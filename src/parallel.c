#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#include "serial_lib.c"

// Static variables
int kernel_row, kernel_col, target_row, target_col, num_targets;
int rank;
int world_size;
int container_size;
int *matrix_ranges;
Matrix kernel;
Matrix *target_container;
FILE *fptr;


void sanity_check() {
    printf("<%d> kernel %d %d | target %d %d | num_targets %d %d | cont %d\n",
        rank,
        kernel_row, kernel_col,
        target_row, target_col,
        num_targets, num_targets/world_size,
        container_size
    );
}

int cmpfunc(const void *a, const void *b) {
    return *(int*) a - *(int*) b;
}


void process_convolution() {
    matrix_ranges = (int*) malloc(sizeof(int) * container_size);

    #pragma omp parallel num_threads(container_size)
    {
        int thread_size, thread_id;
        thread_size = omp_get_num_threads();
        thread_id   = omp_get_thread_num();

        target_container[thread_id] = convolution(&kernel, &(target_container[thread_id]));
        matrix_ranges[thread_id]    = get_matrix_datarange(&(target_container[thread_id]));
    }
}

void init_broadcast_routine() {
    MPI_Bcast(&kernel_row, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&kernel_col, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&target_row, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&target_col, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&num_targets, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (rank != 0)
        init_matrix(&kernel, kernel_row, kernel_col);

    // Broadcasting kernel
    for (int i = 0; i < kernel_row; i++)
        MPI_Bcast(&(kernel.mat[i]), kernel_col, MPI_INT, 0, MPI_COMM_WORLD);
}

void distribute_target_matrix() {
    if (rank == 0) {
        for (int i = 0; i < num_targets; i++) {
            Matrix temp = input_matrix(fptr, target_row, target_col);
            int rank_target = i / (num_targets / world_size);

            if (rank_target == 0)
                target_container[i] = temp;
            else {
                if (rank_target == world_size)
                    rank_target -= 1;

                for (int j = 0; j < target_row; j++)
                    MPI_Send(&(temp.mat[j]), target_col, MPI_INT, rank_target, 0, MPI_COMM_WORLD);
            }
        }
    }
    else {
        for (int i = 0; i < container_size; i++) {
            init_matrix(&(target_container[i]), target_row, target_col);
            for (int j = 0; j < target_row; j++) {
                MPI_Status retcode;
                MPI_Recv(&(target_container[i].mat[j]), target_col, MPI_INT, 0, 0, MPI_COMM_WORLD, &retcode);
            }
        }
    }
}

void merge_and_summary_result() {
    if (rank == 0) {
        // Penerimaan hasil dengan MPI
        int *full_ranges = (int*) malloc(sizeof(int) * num_targets);
        for (int i = 0; i < num_targets; i++) {
            int rank_target = i / (num_targets / world_size);

            if (rank_target == 0)
                full_ranges[i] = matrix_ranges[i];
            else {
                if (rank_target == world_size)
                    rank_target -= 1;

                MPI_Status retcode;
                MPI_Recv(&(full_ranges[i]), 1, MPI_INT, rank_target, 0, MPI_COMM_WORLD, &retcode);
            }
        }

        // Summary
        qsort(full_ranges, num_targets, sizeof(int), cmpfunc);
        int median       = get_median(full_ranges, num_targets);
        int floored_mean = get_floored_mean(full_ranges, num_targets);

        printf("%d\n%d\n%d\n%d\n",
            full_ranges[0],
            full_ranges[num_targets - 1],
            median,
            floored_mean);
    }
    else {
        // Pengiriman hasil dengan MPI
        for (int i = 0; i < container_size; i++)
            MPI_Send(&(matrix_ranges[i]), 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
    }
}


int main(int argc, char *argv[]) {
    clock_t timer;
    timer = clock();
    MPI_Init(NULL, NULL);

    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    // Baca file test case
    if (rank == 0) {
        fptr = fopen(argv[1], "r");
        if (!fptr) {
            printf("Cannot open file\n");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }

        fscanf(fptr, "%d %d", &kernel_row, &kernel_col);
        kernel = input_matrix(fptr, kernel_row, kernel_col);
        fscanf(fptr, "%d %d %d", &num_targets, &target_row, &target_col);
    }

    // Broadcasting metadata dan matriks kernel
    init_broadcast_routine();

    // Distribusi matriks target ke proses
    // Asumsi : Banyak matriks target >= proses
    if (rank != world_size - 1)
        container_size = num_targets / world_size;
    else
        container_size = num_targets - (world_size - 1) * (num_targets / world_size);
    target_container = (Matrix*) malloc(container_size * sizeof(Matrix));

    distribute_target_matrix();

    // Proses konvolusi dengan OpenMP
    process_convolution();

    // Merge dan tampilkan summary hasil konvolusi
    merge_and_summary_result();

    MPI_Finalize();
    if (rank == 0 && argc > 2) {
        timer = clock() - timer;
        double time_elapsed = ((double) timer) / CLOCKS_PER_SEC;
        printf("Time elapsed %f\n", time_elapsed);
    }
    return 0;
}
