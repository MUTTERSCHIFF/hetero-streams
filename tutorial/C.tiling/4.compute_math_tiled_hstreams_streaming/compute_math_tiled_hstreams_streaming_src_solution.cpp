/*
 *    Copyright 2014-2016 Intel Corporation.
 *
 *    This file is subject to the Intel Sample Source Code License. A copy
 *    of the Intel Sample Source Code License is included.
 *
 */

#include <hStreams_app_api.h> //user level APIs
/*
 * This program is slight improved hstreams implementation of the application
 * shown in example in folder 2. It uses only 4 streams, and splits the original
 * computation loops. It keeps the parallel i loop on host and the rest as a MIC
 * side computation. The body of the original i loop is enqueued on the streams
 * in a round robin fashion, after creating the buffer. At the end, it transfers
 * the results back from MIC to source/host and verify the correctness.
 */

// Set number of streams you want to create and over-subscription level.
// Using multiple streams
int streams_per_domain = 4;
int oversubscription = 1; //no over-subscription, i.e., one logical stream
//per physical stream.


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>
#include <omp.h>

typedef float REAL;
// restrict means the pointer does not overlap with others.
typedef REAL *restrict REAL_POINTER;
#define NUM_TESTS_ITERS 4 // Number of times you want to run the program. Running multiple
// times help to settle the running time and almost always
// the first run takes more time than others.

int tile_size = 1; //  Default tile_size is 1.

/* returns in the unit of ms */
double GetTime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1e3 + tv.tv_usec / 1e3;
}

int main(int argc, char **argv)
{
    int dimX = 4;
    int dimY = 64;
    int dimZ = 64;
    int numIters = 64;

    // Process args from command line*/
    int argi = 1;
    int errFlag = 0;
    while (argi < argc) {
        char *one = argv[argi];
        if (!strcmp(one, "-d") && argc > argi + 3) {
            dimX = atoi(argv[argi + 1]);
            dimY = atoi(argv[argi + 2]);
            dimZ = atoi(argv[argi + 3]);
            argi += 4;
        } else if (!strcmp(one, "-n") && argc > argi + 1) {
            numIters = atoi(argv[argi + 1]);
            /* Make numIters a multiple of 16 */
            numIters = numIters & (~0xf);
            argi += 2;
        }
        // Also take tile_size from command line*/
        else if (!strcmp(one, "-t") && argc > argi + 1) {
            tile_size = atoi(argv[argi + 1]);
            if (dimY % tile_size != 0) {
                printf("The given tile_size does not divide dimY evenly!\n");
                return -1;
            }
            argi += 2;
        } else {
            errFlag = 1;
            break;
        }
    }
    if (errFlag) {
        printf("Usage: %s [-d [the size of each dimension]] "
               "[-n [the number of iterations in the kernel]] "
               "[-t tile_size]\n", argv[0]);
        return -1;
    }

    if (dimY != dimZ) {
        printf("DimY is not the same as DimZ, "
               "changing dimZ to make them alike!\n");
        dimZ = dimY;
    }

    if (dimX * dimY * dimZ * numIters % 64 != 0) {
        printf("The production of DimX, DimY, DimZ and "
               "#iters must be multiples of 64!\n");
        return -1;
    }

    printf("DimX=%d, DimY=%d, DimZ=%d, #Loop_Iterations=%d, tile_size=%d\n",
           dimX, dimY, dimZ, numIters, tile_size);

    REAL *out = NULL;

    // 64 bytes aligned allocation.
    posix_memalign((void **) &out, 64,
                   dimX * dimY * dimZ * numIters * sizeof(REAL) * 2);

    if (out == NULL) {
        printf("Memory allocation failed!\n");
        return -1;
    }

    double iterTimes[NUM_TESTS_ITERS];
    double mintime = 1e6;

    // Notice how the out1 and out2 pointers are calculated.
    REAL *out1 = out;
    REAL *out2 = out + dimX * dimY * dimZ * numIters;

    //--------------------------------------------------------------

    // Initialize hStreams with the given StreamsPerDomain and
    // over-subscription level.
    CHECK_HSTR_RESULT(hStreams_app_init(streams_per_domain, oversubscription));

    //--------------------------------------------------------------

    //--------------------------------------------------------------
    // Saving buffer length in a temporary variable to save computation.
    long long buffer_len = dimX * dimY * dimZ * numIters
                           * sizeof(REAL) * 2;

    // Set up buffers.
    // Wrap out with a buffer.
    hStreams_app_create_buf((void *) out, buffer_len);

    register long int buff_length = dimY * dimZ * numIters * sizeof(REAL);

    //!!a Creating two dimX buffer addresses pointing inside out buffer.
    // out1_addr[i] points to the position of out1 buffer where
    // iteration i of the outer loop of the compute kernel writes.
    // out2_addr[i] points to the position of out2 buffer where
    // iteration i of the outer loop of the compute kernel writes.
    REAL *out1_addr[dimX];
    REAL *out2_addr[dimX];

    // Separation between out1 and out2.
    long int out_length = dimX * dimY * dimZ * numIters;

    for (int i = 0; i < dimX; i++) {
        //%% EXERCISE: go back to compute kernel and find out what is the value for
        //   out1_addr[i] and out2_addr[i].
        out1_addr[i] = out + i * dimY * dimZ * numIters;
        out2_addr[i] = out1_addr[i] + out_length;
        //  You can also create buffer in this way instead of creating
        //   out buffer as a whole
        //  hStreams_app_create_buf((void *)out1_addr[i], buff_length);
        //  hStreams_app_create_buf((void *)out2_addr[i], buff_length);
    }

    // Event pointers to support asynchronous function calls.
    HSTR_EVENT eout1[numIters][dimX], eout2[numIters][dimX],
               eout3[numIters][dimX], eout4[numIters][dimX], eout5[numIters][dimX];

    //--------------------------------------------------------------

    for (int i = 0; i < NUM_TESTS_ITERS; i++) {
        iterTimes[i] = GetTime();
        //--------------------------------------------------------------
        // Call device side API.
        // Prepare to perform computation on the sink-side.
        // Outer loop.
        // The body of the original i loop is enqueued on the streams in a round
        // robin fashion.
        for (int ii = 0; ii < dimX; ii++) {


            int stream = ii % streams_per_domain;
            //--------------------------------------------------------------
            // Initialize data at the sink-side

            //!!b  Initializes intermediate data at sink, only
            // the portion  being worked on
            hStreams_app_memset(stream, out1_addr[ii], // source proxy address to write
                                0.5, buff_length, // number of bytes to send
                                &eout4[i][ii]); // completion event

            //%% EXERCISE: Initialize out2_addr[ii] buffer similarly on the sink-side.
            hStreams_app_memset(stream, out2_addr[ii], // source proxy address to write
                                0.5, buff_length, // number of bytes to send
                                &eout5[i][ii]); // completion event


            uint64_t args[8];

            // Pack scalar arguments first, then heap args.
            //%% EXERCISE: Setup the heap args properly.
            args[0] = (uint64_t)(ii);
            args[1] = (uint64_t)(dimX);
            args[2] = (uint64_t)(dimY);
            args[3] = (uint64_t)(dimZ);
            args[4] = (uint64_t)(numIters);
            args[5] = (uint64_t)(tile_size);
            args[6] = (uint64_t)(out1_addr[ii]);
            args[7] = (uint64_t)(out2_addr[ii]);
            //--------------------------------------------------------------

            hStreams_app_invoke(stream, // same idea
                                "compute", // remote function name
                                6, // scalar arg
                                2, // heap args
                                args, // array of args
                                &eout1[i][ii], NULL, // return variable
                                0);

            //-----------------------------------------------------------------
            // Collect result.
            hStreams_app_xfer_memory(stream, out1_addr[ii], // source proxy address to write
                                     out1_addr[ii], // source proxy address to read
                                     buff_length, // number of bytes to send
                                     HSTR_SINK_TO_SRC, // transfer direction
                                     &eout2[i][ii]); // completion event

            //--------------------------------------------------------------
            //!!c Transfer output data from sink to source
            //%% EXERCISE: Transfer data back for out2_addr[ii].
            hStreams_app_xfer_memory(stream, out2_addr[ii], // source proxy address to write
                                     out2_addr[ii], // source proxy address to read
                                     buff_length, // number of bytes to send
                                     HSTR_SINK_TO_SRC, // transfer direction
                                     &eout3[i][ii]); // completion event
        }
        //--------------------------------------------------------------

        // Synchronize.
        CHECK_HSTR_RESULT(hStreams_app_thread_sync());
        //--------------------------------------------------------------

        iterTimes[i] = GetTime() - iterTimes[i];

        double result = out1[numIters / 2] + out2[numIters / 2];
        printf("Test %d takes %.3lf ms with result %.3lf\n", i, iterTimes[i],
               result);
        if (iterTimes[i] < mintime) {
            mintime = iterTimes[i];
        }
    }
    printf("Test's min time is %.3lf ms\n", mintime);

    //--------------------------------------------------------------
    // Cleanup before exiting.
    CHECK_HSTR_RESULT(hStreams_app_fini());
    //--------------------------------------------------------------
    free(out);
    return 0;
}

