#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cassert>
#include <cmath>
#include <unistd.h>
#include "clhelp.h"

int int_compar(const void *x, const void *y)
{
	int i_x = *((int*)x);
	int i_y = *((int*)y);
	return i_x > i_y;
}

void rsort_scan(cl_command_queue &queue,
		cl_context &context,
		cl_kernel &scan_kern,
		cl_kernel &update_kern,
		cl_mem &in_zeroes,
		cl_mem &in_ones, 
		cl_mem &zeroes,
		cl_mem &ones, 
		int v,
		int k,
		int len);

void rsort_reassemble(cl_command_queue &queue,
				cl_context &context,
				cl_kernel &reassemble_kern,
				cl_mem &in, 
				cl_mem &out,
				cl_mem &zeroes,
				cl_mem &ones,
				int k,
				int len);

void cpu_rscan(int *in, int *out, int v, int k, int n)
{
	int t = (in[0] >> k) & 0x1;
	out[0] = (t==v);
	for(int i = 1; i < n; i++)
		{
			t = (in[i] >> k) & 0x1;
			out[i] = out[i-1]+(t==v);
		}
}


int main(int argc, char *argv[])
{
	std::string kernel_source_str;
	
	std::string arraycompact_kernel_file = 
		std::string("radixsort.cl");
	
	std::list<std::string> kernel_names;
	std::string scan_name_str = std::string("scan");
	std::string update_name_str = std::string("update");
	std::string reassemble_name_str = std::string("reassemble");

	kernel_names.push_back(scan_name_str);
	kernel_names.push_back(update_name_str);
	kernel_names.push_back(reassemble_name_str);

	cl_vars_t cv; 
	
	std::map<std::string, cl_kernel> 
		kernel_map;

	int c;
	int n = (1<<20);  
	int *in, *out;
	int *c_scan;
	int n_out=-1;
	bool silent = false;

	while((c = getopt(argc, argv, "n:s:"))!=-1)
		{
			switch(c)
	{
	case 'n':
		n = 1 << atoi(optarg);
		break;
	case 's':
		silent = atoi(optarg) == 1;
		break;
	}
		}

	in = new int[n];
	out = new int[n];
	c_scan = new int[n];
 
	bzero(out, sizeof(int)*n);
	bzero(c_scan, sizeof(int)*n);

	srand(5);
	for(int i = 0; i < n; i++)
		{
			in[i] = rand();
		}

	readFile(arraycompact_kernel_file,
		 kernel_source_str);
	
	initialize_ocl(cv);

	compile_ocl_program(kernel_map, cv, 
					kernel_source_str.c_str(),
					kernel_names);
	
	cl_mem g_in, g_zeros, g_ones, g_out;
	cl_mem g_temp;
	
	cl_int err = CL_SUCCESS;
	g_in = clCreateBuffer(cv.context,CL_MEM_READ_WRITE,
					 sizeof(int)*n,NULL,&err);
	CHK_ERR(err);  

	g_ones = clCreateBuffer(cv.context,CL_MEM_READ_WRITE,
				sizeof(int)*n,NULL,&err);
	CHK_ERR(err);

	g_zeros = clCreateBuffer(cv.context,CL_MEM_READ_WRITE,
				sizeof(int)*n,NULL,&err);
	CHK_ERR(err);
	
	g_out = clCreateBuffer(cv.context,CL_MEM_READ_WRITE,
			 sizeof(int)*n,NULL,&err);
	CHK_ERR(err);
	
	//copy data from host CPU to GPU
	err = clEnqueueWriteBuffer(cv.commands, g_in, true, 0, sizeof(int)*n,
					 in, 0, NULL, NULL);
	CHK_ERR(err);

	err = clEnqueueWriteBuffer(cv.commands, g_out, true, 0, sizeof(int)*n,
					 c_scan, 0, NULL, NULL);
	CHK_ERR(err);
	
	size_t global_work_size[1] = {n};
	size_t local_work_size[1] = {128};

	adjustWorkSize(global_work_size[0], local_work_size[0]);
	global_work_size[0] = std::max(local_work_size[0], global_work_size[0]);
	int left_over = 0;

	double t0 = timestamp();
  /*
  outer level loop that iterates through the bits.
  call kernels on that specific k-th bit.
  */
	for(int k = 0; k < 32; k++){
		//scan zeroes and ones
		rsort_scan(cv.commands,
			cv.context,
			kernel_map[scan_name_str],
			kernel_map[update_name_str],
			g_in, 
			g_in,
			g_zeros, 
			g_ones,
			0,
			k,
			n);

		rsort_reassemble(cv.commands,
			cv.context,
			kernel_map[reassemble_name_str],
			g_in,
			g_out,
			g_zeros, 
			g_ones,
			k,
			n);

    // reassigned pointers due to it not being an
    // in-place sort
		g_temp = g_in;
		g_in = g_out;
		g_out = g_temp;
	}
	t0 = timestamp() - t0;

	/* Sort array on CPU for comparison */
	double t1 = timestamp();
	qsort(in, n, sizeof(int), int_compar);
	t1 = timestamp() - t1;

	err = clEnqueueReadBuffer(cv.commands, g_in, true, 0, sizeof(int)*n,
					out, 0, NULL, NULL);
	CHK_ERR(err);

 for(int i = 0; i < n; i++)
		{
			if(in[i] != out[i])
	{
		if(!silent)
			printf("not sorted @ %d: %d vs %d!\n", i, in[i], out[i]);
		goto done;
	}
		}
 if(!silent)
	 printf("array sorted\n");

 if(silent)
	 {
		 printf("%d,%g,%g\n",n,t1,t0);
	 }
 else
	 {
		 printf("GPU: array of length %d sorted in %g seconds\n", n, t0);

		 printf("CPU: array of length %d sorted in %g seconds\n", n, t1);
	 }
 done:

	clReleaseMemObject(g_in); 
	clReleaseMemObject(g_out);
	clReleaseMemObject(g_ones);
	clReleaseMemObject(g_zeros);
	

	uninitialize_ocl(cv);

	delete [] in;
	delete [] out;
	delete [] c_scan;
	return 0;
}

/* CS194: This function performs an
 * additive prefix scan combined
 * with the masking function needed
 * for radix sort */
 /*
 same recursive scan from homework 5 with the 
 with the modification of handling both
 zeros and ones scan simultaneously in the work
 item to reduce overhead costs.
 */
void rsort_scan(cl_command_queue &queue,
		cl_context &context,
		cl_kernel &scan_kern,
		cl_kernel &update_kern,
		cl_mem &in_zeroes,
		cl_mem &in_ones,
		cl_mem &zeroes,
		cl_mem &ones, 
		int v,
		int k,
		int len)
{
	size_t global_work_size[1] = {len};
	size_t local_work_size[1] = {128};
	int left_over = 0;
	cl_int err;
	
	adjustWorkSize(global_work_size[0], local_work_size[0]);
	global_work_size[0] = std::max(local_work_size[0], global_work_size[0]);

	left_over = global_work_size[0] / local_work_size[0];
	
	cl_mem g_bzeroes = clCreateBuffer(context,CL_MEM_READ_WRITE, 
					sizeof(int)*left_over,NULL,&err);
	CHK_ERR(err);
	cl_mem g_bones = clCreateBuffer(context,CL_MEM_READ_WRITE, 
					sizeof(int)*left_over,NULL,&err);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 0, sizeof(cl_mem), &in_zeroes);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 1, sizeof(cl_mem), &in_ones);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 2, sizeof(cl_mem), &zeroes);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 3, sizeof(cl_mem), &ones);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 4, sizeof(cl_mem), &g_bzeroes);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 5, sizeof(cl_mem), &g_bones);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 6, 2*local_work_size[0]*sizeof(cl_int), NULL);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 7, 2*local_work_size[0]*sizeof(cl_int), NULL);
	CHK_ERR(err);

	/* CS194: v will be either 0 or 1 in order to perform
	 * a scan of bits set (or unset) */
	err = clSetKernelArg(scan_kern, 8, sizeof(int), &v);
	CHK_ERR(err);

	/* CS194: the current bit position (0 to 31) that
	 * we want to operate on */
	err = clSetKernelArg(scan_kern, 9, sizeof(int), &k);
	CHK_ERR(err);

	err = clSetKernelArg(scan_kern, 10, sizeof(int), &len);
	CHK_ERR(err);

	err = clEnqueueNDRangeKernel(queue,
						 scan_kern,
						 1,//work_dim,
						 NULL, //global_work_offset
						 global_work_size, //global_work_size
						 local_work_size, //local_work_size
						 0, //num_events_in_wait_list
						 NULL, //event_wait_list
						 NULL //
						 );
	CHK_ERR(err);

	if(left_over > 1)
		{
			cl_mem g_bbzeroes = clCreateBuffer(context,CL_MEM_READ_WRITE, 
							sizeof(int)*left_over,NULL,&err);

			 cl_mem g_bbones = clCreateBuffer(context,CL_MEM_READ_WRITE, 
							sizeof(int)*left_over,NULL,&err);

			/* Recursively perform scan if needed */
			rsort_scan(queue, context, scan_kern, update_kern, g_bzeroes, g_bones,
				 g_bbzeroes, g_bbones, -1, k, left_over);

			err = clSetKernelArg(update_kern,0,
				 sizeof(cl_mem), &zeroes);
			CHK_ERR(err);
			err = clSetKernelArg(update_kern,1,
				 sizeof(cl_mem), &ones);
			CHK_ERR(err);
			
			err = clSetKernelArg(update_kern,2,
				 sizeof(cl_mem), &g_bbzeroes);
			CHK_ERR(err);

			err = clSetKernelArg(update_kern,3,
				 sizeof(cl_mem), &g_bbones);
			CHK_ERR(err);

			err = clSetKernelArg(update_kern,4,
				 sizeof(int), &len);
			CHK_ERR(err);
			
			/* Update partial scans */
			err = clEnqueueNDRangeKernel(queue,
					 update_kern,
					 1,//work_dim,
					 NULL, //global_work_offset
					 global_work_size, //global_work_size
					 local_work_size, //local_work_size
					 0, //num_events_in_wait_list
					 NULL, //event_wait_list
					 NULL //
					 );
			CHK_ERR(err);
			
			clReleaseMemObject(g_bbzeroes);
			clReleaseMemObject(g_bbones);
		}

	clReleaseMemObject(g_bzeroes);
	clReleaseMemObject(g_bones);
}

/*
enqueues the input and output arrays and the scanned
 zeros and ones. outputs the sorted array by the
 k-th bit
*/
void rsort_reassemble(cl_command_queue &queue,
				cl_context &context,
				cl_kernel &reassemble_kern,
				cl_mem &in, 
				cl_mem &out,
				cl_mem &zeroes,
				cl_mem &ones,
				int k,
				int len)
{
	size_t global_work_size[1] = {len};
	size_t local_work_size[1] = {128};
	cl_int err;

	adjustWorkSize(global_work_size[0], local_work_size[0]);
	global_work_size[0] = std::max(local_work_size[0], global_work_size[0]);

	err = clSetKernelArg(reassemble_kern, 0, sizeof(cl_mem), &in);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 1, sizeof(cl_mem), &out);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 2, sizeof(cl_mem), &zeroes);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 3, sizeof(cl_mem), &ones);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 4, local_work_size[0]*sizeof(cl_int), NULL);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 5, local_work_size[0]*sizeof(cl_int), NULL);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 6, local_work_size[0]*sizeof(cl_int), NULL);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 7, sizeof(int), &k);
	CHK_ERR(err);

	err = clSetKernelArg(reassemble_kern, 8, sizeof(int), &len);
	CHK_ERR(err);

	err = clEnqueueNDRangeKernel(queue,
						 reassemble_kern,
						 1,//work_dim,
						 NULL, //global_work_offset
						 global_work_size, //global_work_size
						 local_work_size, //local_work_size
						 0, //num_events_in_wait_list
						 NULL, //event_wait_list
						 NULL //
						 );
	CHK_ERR(err);
}
