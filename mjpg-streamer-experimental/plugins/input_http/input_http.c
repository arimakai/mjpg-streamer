/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
# Modified by Eugene Katsevman, 2011                                           #
# Modified by Carlos Garcia Saura, 2018                                        #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#include "mjpg-proxy.h"
#include "libepeg/Epeg.h"

/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;
static pthread_mutex_t controls_mutex;
static int plugin_number;

void *worker_thread(void *);
void worker_cleanup(void *);
int init_opencl();

#define INPUT_PLUGIN_NAME "HTTP Input plugin"

struct extractor_state  proxy;

/*** plugin interface functions ***/

/******************************************************************************
Description.: parse input parameters
Input Value.: param contains the command line string and a pointer to globals
Return Value: 0 if everything is ok
******************************************************************************/


int input_init(input_parameter *param, int plugin_no)
{
    int i;

    if(pthread_mutex_init(&controls_mutex, NULL) != 0) {
        IPRINT("could not initialize mutex variable\n");
        exit(EXIT_FAILURE);
    }

    param->argv[0] = INPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }
    init_mjpg_proxy( &proxy );

    reset_getopt();
    if (parse_cmd_line(&proxy, param->argc, param->argv))
       return 1;

    pglobal = param->global;

    IPRINT("host.............: %s\n", proxy.hostname);
    IPRINT("path.............: %s\n", proxy.path);
    IPRINT("port.............: %s\n", proxy.port);
    if(proxy.width > 0 && proxy.height > 0) {
        IPRINT("rescale width....: %d\n", proxy.width);
        IPRINT("rescale height...: %d\n", proxy.height);
        IPRINT("rescale quality..: %d\n", proxy.quality);
        IPRINT("rescale opencl...: %d\n", proxy.opencl);
    }

    return 0;
}

/******************************************************************************
Description.: stops the execution of the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
int input_stop(int id)
{
    DBG("will cancel input thread\n");
    pthread_cancel(worker);
    return 0;
}

/******************************************************************************
Description.: starts the worker thread and allocates memory
Input Value.: -
Return Value: 0
******************************************************************************/
int input_run(int id)
{
    pglobal->in[id].buf = malloc(256 * 1024);
    if(pglobal->in[id].buf == NULL) {
        fprintf(stderr, "could not allocate memory\n");
        exit(EXIT_FAILURE);
    }

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }
    pthread_detach(worker);

    return 0;
}

/******************************************************************************
Description.: rescales the received JPG (if needed) and writes it to the global buffer
Input Value.: raw image data
Return Value: -
******************************************************************************/
void on_image_received(char * data, int length){
        pthread_mutex_lock(&pglobal->in[plugin_number].db);

        if(proxy.width > 0 && proxy.height > 0) { /* only activate the epeg library when resizing is needed */
            /* initialize the epeg structure loading image metadata */
            Epeg_Image * raw_img = epeg_memory_open(data, length);

            if(raw_img) { /* corrupt frames will be discarded */
                /* tell the epeg library what needs to be modified */
                epeg_quality_set(raw_img, proxy.quality);
                epeg_decode_size_set(raw_img, proxy.width,proxy.height);

                /* assign the output buffer directly as the global buffer */
                epeg_memory_output_set(raw_img, &(pglobal->in[plugin_number].buf), &(pglobal->in[plugin_number].size));

                /* apply the transformations and write to the output buffer */
                if(proxy.opencl == 1 && proxy.cl_obj.err == CL_SUCCESS) {
                    epeg_encode_opencl(raw_img, &proxy.cl_obj.context, &proxy.cl_obj.kernel_scale, &proxy.cl_obj.queue);
                } else {
                    epeg_encode(raw_img);
                }

                /* free the internal allocations of the epeg library */
                epeg_close(raw_img);
            }
        } else {
            /* directly copy JPG picture to global buffer */
            pglobal->in[plugin_number].size = length;
            memcpy(pglobal->in[plugin_number].buf, data, pglobal->in[plugin_number].size);
        }
        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);

}

int init_opencl()
{
    IPRINT("Start initial opencl\n");

    // Create OpenCL program
    const char* source =
        "__kernel void scale(__global unsigned char* src, __global unsigned char* dst, const int output_components, \n"
        "const int input_width, const int input_height, const int out_w, const int out_h) {\n"
        "   int x = get_global_id(0);\n"
        "   int y = get_global_id(1);\n"
        "   if (x >= input_width || y >= input_height) return;\n"
        "   unsigned char* row = src + (((y * input_height) / out_h) * output_components * input_width);\n"
        "   unsigned char* pixel = dst + (y * output_components * input_width) + (x * output_components);\n"
        "   unsigned char* src_pixel = row + (((x * input_width) / out_w) * output_components);\n"
        "   for (int i = 0; i < output_components; i++) {\n"
        "       pixel[i] = src_pixel[i];\n"
        "   }\n"
        "}\n";

    // Get OpenCL platform
    proxy.cl_obj.err = clGetPlatformIDs(1, &proxy.cl_obj.platform, &proxy.cl_obj.num_platforms);
    if (proxy.cl_obj.err != CL_SUCCESS) {
        // Handle error
        return -1;
    }

    // Get device
    proxy.cl_obj.err = clGetDeviceIDs(proxy.cl_obj.platform, CL_DEVICE_TYPE_GPU, 1, &proxy.cl_obj.device, &proxy.cl_obj.num_devices);
    if (proxy.cl_obj.err != CL_SUCCESS) {
        // Handle error
        return -1;
    }

    // Create context
    proxy.cl_obj.context = clCreateContext(NULL, 1, &proxy.cl_obj.device, NULL, NULL, &proxy.cl_obj.err);
    if (proxy.cl_obj.err != CL_SUCCESS) {
        // Handle error
        return -1;
    }

    // Create command queue
    proxy.cl_obj.queue = clCreateCommandQueue(proxy.cl_obj.context, proxy.cl_obj.device, 0, &proxy.cl_obj.err);
    if (proxy.cl_obj.err != CL_SUCCESS) {
        // Handle error
        clReleaseContext(proxy.cl_obj.context);

        return -1;
    }

    // Build the OpenCL program
    proxy.cl_obj.program = clCreateProgramWithSource(proxy.cl_obj.context, 1, &source, NULL, &proxy.cl_obj.err);
    if (proxy.cl_obj.err != CL_SUCCESS) {
        IPRINT("Error creating program\n");
        clReleaseCommandQueue(proxy.cl_obj.queue);
        clReleaseContext(proxy.cl_obj.context);
        return -1;
    }

    // Build the program
    proxy.cl_obj.err = clBuildProgram(proxy.cl_obj.program, 1, &proxy.cl_obj.device, NULL, NULL, NULL);
    if (proxy.cl_obj.err != CL_SUCCESS) {
        // Handle error
        IPRINT("Error building program\n");
        clReleaseProgram(proxy.cl_obj.program);
        clReleaseCommandQueue(proxy.cl_obj.queue);
        clReleaseContext(proxy.cl_obj.context);
        return -1;
    }

    // Create kernel
    proxy.cl_obj.kernel_scale = clCreateKernel(proxy.cl_obj.program, "scale", &proxy.cl_obj.err);
    if (proxy.cl_obj.err != CL_SUCCESS) {
        // Handle error
        IPRINT("Error creating kernel\n");
        clReleaseProgram(proxy.cl_obj.program);
        clReleaseCommandQueue(proxy.cl_obj.queue);
        clReleaseContext(proxy.cl_obj.context);
        return -1;
    }

    return 0;
}

void *worker_thread(void *arg)
{

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    if(proxy.opencl == 1) {
        IPRINT("Use OpenCL with rescale image\n");
        if(init_opencl() != 0) {
            IPRINT("failed to initial opencl\n");
        }
    }

    proxy.on_image_received = on_image_received;
    proxy.should_stop =  & pglobal->stop;
    connect_and_stream(&proxy);

    IPRINT("leaving input thread, calling cleanup function now\n");
    pthread_cleanup_pop(1);

    return NULL;
}

/******************************************************************************
Description.: this functions cleans up allocated resources
Input Value.: arg is unused
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    if(proxy.opencl == 1 && proxy.cl_obj.err == CL_SUCCESS) {
        clReleaseKernel(proxy.cl_obj.kernel_scale);
        clReleaseProgram(proxy.cl_obj.program);
        clReleaseCommandQueue(proxy.cl_obj.queue);
        clReleaseContext(proxy.cl_obj.context);
    }

    first_run = 0;
    DBG("cleaning up resources allocated by input thread\n");
    close_mjpg_proxy(&proxy);
    if(pglobal->in[plugin_number].buf != NULL) free(pglobal->in[plugin_number].buf);
}




