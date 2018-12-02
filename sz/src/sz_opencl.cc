#include "sz_opencl.h"
#include "sz.h"
#include <algorithm>
#include <iterator>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#define __CL_ENABLE_EXCEPTIONS
#include "CL/cl.hpp"

struct sz_opencl_state
{
  struct error_t
  {
    cl_int code = 0;
    const char* str = nullptr;
  } error;
  cl::Platform platform;
  cl::Device device;
  cl::Context context;
  cl::CommandQueue queue;
};

extern "C"
{
  int sz_opencl_init(struct sz_opencl_state** state)
  {
    try {
      *state = new sz_opencl_state;

      std::vector<cl::Platform> platforms;
      cl::Platform::get(&platforms);

      auto valid_platform =
        std::find_if(std::begin(platforms), std::end(platforms),
                     [state](cl::Platform const& platform) {
                       try {
                         std::vector<cl::Device> devices;
                         platform.getDevices(CL_DEVICE_TYPE_GPU, &devices);
                         (*state)->device = devices.front();
                         (*state)->platform = platform;
                         return true;
                       } catch (cl::Error const& error) {
                         if (error.err() != CL_DEVICE_NOT_FOUND)
                           throw;
                       }
                       return false;
                     });
      if (valid_platform == std::end(platforms))
        throw cl::Error(CL_DEVICE_NOT_FOUND, "Failed to find a GPU");

      (*state)->context = cl::Context({ (*state)->device });
      (*state)->queue = cl::CommandQueue((*state)->context, (*state)->device);

      return SZ_SCES;
    } catch (cl::Error const& cl_error) {
      (*state)->error.code = cl_error.err();
      (*state)->error.str = cl_error.what();
      return SZ_NSCS;

    } catch (...) {
      delete *state;
      *state = nullptr;
      return SZ_NSCS;
    }
  }

  int sz_opencl_release(struct sz_opencl_state** state)
  {
    delete *state;

    return SZ_SCES;
  }

  const char* sz_opencl_error_msg(struct sz_opencl_state* state)
  {
    if (state == nullptr) {
      return "sz opencl allocation failed";
    }

    return state->error.str;
  }

  int sz_opencl_error_code(struct sz_opencl_state* state)
  {
    if (state == nullptr) {
      return -1;
    }

    return state->error.code;
  }

  int sz_opencl_check(struct sz_opencl_state* state)
  {
    try {
      std::string vec_add(
        R"(
				kernel void add(__global float* a, __global float* b, __global float* c)
				{
					int id = get_global_id(0);
					c[id] = a[id] + b[id];
				}
				)");
      cl::Program::Sources sources(
        1, std::make_pair(vec_add.c_str(), vec_add.size() + 1));

      cl::Program program(state->context, sources);
      program.build({ state->device });
      cl::Kernel kernel(program, "add");
      const int size = 1024;
      std::vector<float> h_a(size);
      std::vector<float> h_b(size);
      std::vector<float> h_c(size);
      std::vector<float> verify(size);
      cl::Buffer d_a(state->context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR,
                     sizeof(cl_float) * size);
      cl::Buffer d_b(state->context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR,
                     sizeof(cl_float) * size);
      cl::Buffer d_c(state->context, CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR,
                     sizeof(cl_float) * size);

      auto random_fill = [](std::vector<float>& vec, int seed) {
        std::seed_seq seed_seq{ seed };
        std::mt19937 gen(seed_seq);
        std::uniform_real_distribution<float> dist;

        std::generate(std::begin(vec), std::end(vec),
                      [&dist, &gen]() { return dist(gen); });
      };
      random_fill(h_a, 0);
      random_fill(h_b, 1);
      random_fill(h_c, 2);
      std::transform(std::begin(h_a), std::end(h_a), std::begin(h_b),
                     std::begin(verify),
                     [](float a, float b) { return a + b; });

      kernel.setArg(0, d_a);
      kernel.setArg(1, d_b);
      kernel.setArg(2, d_c);

      state->queue.enqueueWriteBuffer(d_a, /*blocking*/ CL_TRUE, /*offset*/ 0,
                                      /*size*/ sizeof(cl_float) * size,
                                      h_a.data());
      state->queue.enqueueWriteBuffer(d_b, /*blocking*/ CL_TRUE, /*offset*/ 0,
                                      /*size*/ sizeof(cl_float) * size,
                                      h_b.data());
      state->queue.enqueueNDRangeKernel(kernel, /*offset*/ cl::NullRange,
                                        /*global*/ cl::NDRange(size),
                                        cl::NullRange);
      state->queue.finish();
      state->queue.enqueueReadBuffer(d_c, /*blocking*/ CL_TRUE, /*offset*/ 0,
                                     /*size*/ sizeof(cl_float) * size,
                                     h_c.data());

      if (std::equal(std::begin(h_c), std::end(h_c), std::begin(verify),
                     std::end(verify))) {
        return SZ_SCES;
      } else {
        return SZ_NSCS;
      }
    } catch (cl::Error const& error) {
      state->error.code = error.err();
      state->error.str = error.what();
      return SZ_NSCS;
    }
  }

  unsigned char* sz_compress_float3d_opencl(float* oriData, size_t r1,
                                            size_t r2, size_t r3,
                                            double realPrecision,
                                            size_t* comp_size)
  {

    unsigned int quantization_intervals;
    float sz_sample_correct_freq = -1; // 0.5; //-1
    float dense_pos;
    float mean_flush_freq;
    unsigned char use_mean = 0;

    // calculate block dims
    size_t num_x, num_y, num_z;
    size_t block_size = 6;
    num_x = (r1 - 1) / block_size + 1;
    num_y = (r2 - 1) / block_size + 1;
    num_z = (r3 - 1) / block_size + 1;

    size_t max_num_block_elements = block_size * block_size * block_size;
    size_t num_blocks = num_x * num_y * num_z;
    size_t num_elements = r1 * r2 * r3;

    size_t dim0_offset = r2 * r3;
    size_t dim1_offset = r3;

    int* result_type =
      (int*)malloc(num_blocks * max_num_block_elements * sizeof(int));
    size_t unpred_data_max_size = max_num_block_elements;
    float* result_unpredictable_data =
      (float*)malloc(unpred_data_max_size * sizeof(float) * num_blocks);
    size_t total_unpred = 0;
    size_t unpredictable_count;
    float* data_pos = oriData;
    int* type = result_type;
    float* reg_params = (float*)malloc(num_blocks * 4 * sizeof(float));
    float* reg_params_pos = reg_params;
    // move regression part out
    size_t params_offset_b = num_blocks;
    size_t params_offset_c = 2 * num_blocks;
    size_t params_offset_d = 3 * num_blocks;
    float* pred_buffer = (float*)malloc((block_size + 1) * (block_size + 1) *
                                        (block_size + 1) * sizeof(float));
    float* pred_buffer_pos = NULL;
    float* block_data_pos_x = NULL;
    float* block_data_pos_y = NULL;
    float* block_data_pos_z = NULL;
    for (size_t i = 0; i < num_x; i++) {
      for (size_t j = 0; j < num_y; j++) {
        for (size_t k = 0; k < num_z; k++) {
          data_pos = oriData + i * block_size * dim0_offset +
                     j * block_size * dim1_offset + k * block_size;
          pred_buffer_pos = pred_buffer;
          block_data_pos_x = data_pos;
          // use the buffer as block_size*block_size*block_size
          for (size_t ii = 0; ii < block_size; ii++) {
            block_data_pos_y = block_data_pos_x;
            for (size_t jj = 0; jj < block_size; jj++) {
              block_data_pos_z = block_data_pos_y;
              for (size_t kk = 0; kk < block_size; kk++) {
                *pred_buffer_pos = *block_data_pos_z;
                if (k * block_size + kk + 1 < r3)
                  block_data_pos_z++;
                pred_buffer_pos++;
              }
              if (j * block_size + jj + 1 < r2)
                block_data_pos_y += dim1_offset;
            }
            if (i * block_size + ii + 1 < r1)
              block_data_pos_x += dim0_offset;
          }
          /*Calculate regression coefficients*/
          {
            float* cur_data_pos = pred_buffer;
            float fx = 0.0;
            float fy = 0.0;
            float fz = 0.0;
            float f = 0;
            float sum_x, sum_y;
            float curData;
            for (size_t i = 0; i < block_size; i++) {
              sum_x = 0;
              for (size_t j = 0; j < block_size; j++) {
                sum_y = 0;
                for (size_t k = 0; k < block_size; k++) {
                  curData = *cur_data_pos;
                  sum_y += curData;
                  fz += curData * k;
                  cur_data_pos++;
                }
                fy += sum_y * j;
                sum_x += sum_y;
              }
              fx += sum_x * i;
              f += sum_x;
            }
            float coeff = 1.0 / (block_size * block_size * block_size);
            reg_params_pos[0] =
              (2 * fx / (block_size - 1) - f) * 6 * coeff / (block_size + 1);
            reg_params_pos[params_offset_b] =
              (2 * fy / (block_size - 1) - f) * 6 * coeff / (block_size + 1);
            reg_params_pos[params_offset_c] =
              (2 * fz / (block_size - 1) - f) * 6 * coeff / (block_size + 1);
            reg_params_pos[params_offset_d] =
              f * coeff -
              ((block_size - 1) * reg_params_pos[0] / 2 +
               (block_size - 1) * reg_params_pos[params_offset_b] / 2 +
               (block_size - 1) * reg_params_pos[params_offset_c] / 2);
          }
          reg_params_pos++;
        }
      }
    }

    if (exe_params->optQuantMode == 1) {
      quantization_intervals =
        optimize_intervals_float_3D_with_freq_and_dense_pos(
          oriData, r1, r2, r3, realPrecision, &dense_pos,
          &sz_sample_correct_freq, &mean_flush_freq);
      if (mean_flush_freq > 0.5 || mean_flush_freq > sz_sample_correct_freq)
        use_mean = 1;
      updateQuantizationInfo(quantization_intervals);
    } else {
      quantization_intervals = exe_params->intvCapacity;
    }

    float mean = 0;
    if (use_mean) {
      // compute mean
      double sum = 0.0;
      size_t mean_count = 0;
      for (size_t i = 0; i < num_elements; i++) {
        if (fabs(oriData[i] - dense_pos) < realPrecision) {
          sum += oriData[i];
          mean_count++;
        }
      }
      if (mean_count > 0)
        mean = sum / mean_count;
    }

    double tmp_realPrecision = realPrecision;

    // use two prediction buffers for higher performance
    float* unpredictable_data = result_unpredictable_data;
    unsigned char* indicator =
      (unsigned char*)malloc(num_blocks * sizeof(unsigned char));
    memset(indicator, 0, num_blocks * sizeof(unsigned char));
    unsigned char* indicator_pos = indicator;

    int intvCapacity = exe_params->intvCapacity;
    int intvRadius = exe_params->intvRadius;
    float noise = realPrecision * 1.22;
    reg_params_pos = reg_params;

    memset(pred_buffer, 0,
           (block_size + 1) * (block_size + 1) * (block_size + 1) *
             sizeof(float));
    int pred_buffer_block_size = block_size + 1;
    int strip_dim0_offset = pred_buffer_block_size * pred_buffer_block_size;
    int strip_dim1_offset = pred_buffer_block_size;

    // select
    if (use_mean) {
      for (size_t i = 0; i < num_x; i++) {
        for (size_t j = 0; j < num_y; j++) {
          for (size_t k = 0; k < num_z; k++) {
            data_pos = oriData + i * block_size * dim0_offset +
                       j * block_size * dim1_offset + k * block_size;
            // add 1 in x, y, z offset
            pred_buffer_pos = pred_buffer +
                              pred_buffer_block_size * pred_buffer_block_size +
                              pred_buffer_block_size + 1;
            block_data_pos_x = data_pos;
            for (size_t ii = 0; ii < block_size; ii++) {
              block_data_pos_y = block_data_pos_x;
              for (size_t jj = 0; jj < block_size; jj++) {
                block_data_pos_z = block_data_pos_y;
                for (size_t kk = 0; kk < block_size; kk++) {
                  *pred_buffer_pos = *block_data_pos_z;
                  if (k * block_size + kk + 1 < r3)
                    block_data_pos_z++;
                  pred_buffer_pos++;
                }
                // add 1 in z offset
                pred_buffer_pos++;
                if (j * block_size + jj + 1 < r2)
                  block_data_pos_y += dim1_offset;
              }
              // add 1 in y offset
              pred_buffer_pos += pred_buffer_block_size;
              if (i * block_size + ii + 1 < r1)
                block_data_pos_x += dim0_offset;
            }
            /*sampling and decide which predictor*/
            {
              // sample point [1, 1, 1] [1, 1, 4] [1, 4, 1] [1, 4, 4] [4, 1, 1]
              // [4, 1, 4] [4, 4, 1] [4, 4, 4]
              float* cur_data_pos;
              float curData;
              float pred_reg, pred_sz;
              float err_sz = 0.0, err_reg = 0.0;
              int bmi = 0;
              for (size_t i = 2; i <= block_size; i++) {
                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  i * pred_buffer_block_size + i;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * (i - 1) +
                           reg_params_pos[params_offset_c] * (i - 1) +
                           reg_params_pos[params_offset_d];
                err_sz +=
                  std::min(fabs(pred_sz - curData) + noise, fabs(mean - curData));
                err_reg += fabs(pred_reg - curData);

                bmi = block_size - i + 1;
                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  i * pred_buffer_block_size + bmi;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * (i - 1) +
                           reg_params_pos[params_offset_c] * bmi +
                           reg_params_pos[params_offset_d];
                err_sz +=
                  std::min(fabs(pred_sz - curData) + noise, fabs(mean - curData));
                err_reg += fabs(pred_reg - curData);

                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  bmi * pred_buffer_block_size + i;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * bmi +
                           reg_params_pos[params_offset_c] * (i - 1) +
                           reg_params_pos[params_offset_d];
                err_sz +=
                  std::min(fabs(pred_sz - curData) + noise, fabs(mean - curData));
                err_reg += fabs(pred_reg - curData);

                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  bmi * pred_buffer_block_size + bmi;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * bmi +
                           reg_params_pos[params_offset_c] * bmi +
                           reg_params_pos[params_offset_d];
                err_sz +=
                  std::min(fabs(pred_sz - curData) + noise, fabs(mean - curData));
                err_reg += fabs(pred_reg - curData);
              }
              // indicator_pos[k] = (err_sz < err_reg);
              indicator_pos[k] = !(err_reg < err_sz);
            }
            reg_params_pos++;
          } // end k
          indicator_pos += num_z;
        } // end j
      }   // end i
    } else {
      for (size_t i = 0; i < num_x; i++) {
        for (size_t j = 0; j < num_y; j++) {
          for (size_t k = 0; k < num_z; k++) {
            data_pos = oriData + i * block_size * dim0_offset +
                       j * block_size * dim1_offset + k * block_size;
            // add 1 in x, y, z offset
            pred_buffer_pos = pred_buffer +
                              pred_buffer_block_size * pred_buffer_block_size +
                              pred_buffer_block_size + 1;
            block_data_pos_x = data_pos;
            for (size_t ii = 0; ii < block_size; ii++) {
              block_data_pos_y = block_data_pos_x;
              for (size_t jj = 0; jj < block_size; jj++) {
                block_data_pos_z = block_data_pos_y;
                for (size_t kk = 0; kk < block_size; kk++) {
                  *pred_buffer_pos = *block_data_pos_z;
                  if (k * block_size + kk < r3)
                    block_data_pos_z++;
                  pred_buffer_pos++;
                }
                // add 1 in z offset
                pred_buffer_pos++;
                if (j * block_size + jj < r2)
                  block_data_pos_y += dim1_offset;
              }
              // add 1 in y offset
              pred_buffer_pos += pred_buffer_block_size;
              if (i * block_size + ii < r1)
                block_data_pos_x += dim0_offset;
            }
            /*sampling*/
            {
              // sample point [1, 1, 1] [1, 1, 4] [1, 4, 1] [1, 4, 4] [4, 1, 1]
              // [4, 1, 4] [4, 4, 1] [4, 4, 4]
              float* cur_data_pos;
              float curData;
              float pred_reg, pred_sz;
              float err_sz = 0.0, err_reg = 0.0;
              int bmi;
              for (size_t i = 2; i <= block_size; i++) {
                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  i * pred_buffer_block_size + i;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * (i - 1) +
                           reg_params_pos[params_offset_c] * (i - 1) +
                           reg_params_pos[params_offset_d];
                err_sz += fabs(pred_sz - curData) + noise;
                err_reg += fabs(pred_reg - curData);

                bmi = block_size - i + 1;
                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  i * pred_buffer_block_size + bmi;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * (i - 1) +
                           reg_params_pos[params_offset_c] * bmi +
                           reg_params_pos[params_offset_d];
                err_sz += fabs(pred_sz - curData) + noise;
                err_reg += fabs(pred_reg - curData);

                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  bmi * pred_buffer_block_size + i;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * bmi +
                           reg_params_pos[params_offset_c] * (i - 1) +
                           reg_params_pos[params_offset_d];
                err_sz += fabs(pred_sz - curData) + noise;
                err_reg += fabs(pred_reg - curData);

                cur_data_pos =
                  pred_buffer +
                  i * pred_buffer_block_size * pred_buffer_block_size +
                  bmi * pred_buffer_block_size + bmi;
                curData = *cur_data_pos;
                pred_sz =
                  cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset] -
                  cur_data_pos[-strip_dim1_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - 1] -
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                  cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                pred_reg = reg_params_pos[0] * (i - 1) +
                           reg_params_pos[params_offset_b] * bmi +
                           reg_params_pos[params_offset_c] * bmi +
                           reg_params_pos[params_offset_d];
                err_sz += fabs(pred_sz - curData) + noise;
                err_reg += fabs(pred_reg - curData);
              }
              // indicator_pos[k] = (err_sz < err_reg);
              indicator_pos[k] = !(err_reg < err_sz);
            }
            reg_params_pos++;
          }
          indicator_pos += num_z;
        }
      }
    }

    size_t reg_count = 0;
    for (size_t i = 0; i < num_blocks; i++) {
      if (!(indicator[i])) {
        reg_params[reg_count] = reg_params[i];
        reg_params[reg_count + params_offset_b] =
          reg_params[i + params_offset_b];
        reg_params[reg_count + params_offset_c] =
          reg_params[i + params_offset_c];
        reg_params[reg_count + params_offset_d] =
          reg_params[i + params_offset_d];
        reg_count++;
      }
    }
    // Compress coefficient arrays
    double precision_a, precision_b, precision_c, precision_d;
    float rel_param_err = 0.025;
    precision_a = rel_param_err * realPrecision / block_size;
    precision_b = rel_param_err * realPrecision / block_size;
    precision_c = rel_param_err * realPrecision / block_size;
    precision_d = rel_param_err * realPrecision;
    float last_coeffcients[4] = { 0.0 };
    int coeff_intvCapacity_sz = 65536;
    int coeff_intvRadius = coeff_intvCapacity_sz / 2;
    int* coeff_type[4];
    int* coeff_result_type = (int*)malloc(reg_count * 4 * sizeof(int));
    float* coeff_unpred_data[4];
    float* coeff_unpredictable_data =
      (float*)malloc(reg_count * 4 * sizeof(float));
    double precision[4];
    precision[0] = precision_a, precision[1] = precision_b,
    precision[2] = precision_c, precision[3] = precision_d;
    for (int i = 0; i < 4; i++) {
      coeff_type[i] = coeff_result_type + i * reg_count;
      coeff_unpred_data[i] = coeff_unpredictable_data + i * reg_count;
    }
    int coeff_index = 0;
    unsigned int coeff_unpredictable_count[4] = { 0 };

    float* reg_params_separte[4];
    for (int i = 0; i < 4; i++) {
      reg_params_separte[i] = reg_params + i * num_blocks;
    }
    for (size_t i = 0; i < reg_count; i++) {
      // for each coeff
      float cur_coeff;
      double diff, itvNum;
      for (int e = 0; e < 4; e++) {
        cur_coeff = reg_params_separte[e][i];
        diff = cur_coeff - last_coeffcients[e];
        itvNum = fabs(diff) / precision[e] + 1;
        if (itvNum < coeff_intvCapacity_sz) {
          if (diff < 0)
            itvNum = -itvNum;
          coeff_type[e][coeff_index] = (int)(itvNum / 2) + coeff_intvRadius;
          last_coeffcients[e] =
            last_coeffcients[e] +
            2 * (coeff_type[e][coeff_index] - coeff_intvRadius) * precision[e];
          // ganrantee compression error against the case of machine-epsilon
          if (fabs(cur_coeff - last_coeffcients[e]) > precision[e]) {
            coeff_type[e][coeff_index] = 0;
            last_coeffcients[e] = cur_coeff;
            coeff_unpred_data[e][coeff_unpredictable_count[e]++] = cur_coeff;
          }
        } else {
          coeff_type[e][coeff_index] = 0;
          last_coeffcients[e] = cur_coeff;
          coeff_unpred_data[e][coeff_unpredictable_count[e]++] = cur_coeff;
        }
        reg_params_separte[e][i] = last_coeffcients[e];
      }
      coeff_index++;
    }
    // pred & quantization
    int* blockwise_unpred_count = (int*)malloc(num_blocks * sizeof(int));
    int* blockwise_unpred_count_pos = blockwise_unpred_count;
    reg_params_pos = reg_params;
    indicator_pos = indicator;
    if (use_mean) {
      int intvCapacity_sz = intvCapacity - 2;
      type = result_type;
      for (size_t i = 0; i < num_x; i++) {
        for (size_t j = 0; j < num_y; j++) {
          for (size_t k = 0; k < num_z; k++) {
            data_pos = oriData + i * block_size * dim0_offset +
                       j * block_size * dim1_offset + k * block_size;
            // add 1 in x, y, z offset
            pred_buffer_pos = pred_buffer +
                              pred_buffer_block_size * pred_buffer_block_size +
                              pred_buffer_block_size + 1;
            block_data_pos_x = data_pos;
            for (size_t ii = 0; ii < block_size; ii++) {
              block_data_pos_y = block_data_pos_x;
              for (size_t jj = 0; jj < block_size; jj++) {
                block_data_pos_z = block_data_pos_y;
                for (size_t kk = 0; kk < block_size; kk++) {
                  *pred_buffer_pos = *block_data_pos_z;
                  if (k * block_size + kk + 1 < r3)
                    block_data_pos_z++;
                  pred_buffer_pos++;
                }
                // add 1 in z offset
                pred_buffer_pos++;
                if (j * block_size + jj + 1 < r2)
                  block_data_pos_y += dim1_offset;
              }
              // add 1 in y offset
              pred_buffer_pos += pred_buffer_block_size;
              if (i * block_size + ii + 1 < r1)
                block_data_pos_x += dim0_offset;
            }
            if (!(indicator_pos[k])) {
              float curData;
              float pred;
              double itvNum;
              double diff;
              size_t index = 0;
              size_t block_unpredictable_count = 0;
              float* cur_data_pos =
                pred_buffer + pred_buffer_block_size * pred_buffer_block_size +
                pred_buffer_block_size + 1;
              for (size_t ii = 0; ii < block_size; ii++) {
                for (size_t jj = 0; jj < block_size; jj++) {
                  for (size_t kk = 0; kk < block_size; kk++) {
                    curData = *cur_data_pos;
                    pred = reg_params_pos[0] * ii +
                           reg_params_pos[params_offset_b] * jj +
                           reg_params_pos[params_offset_c] * kk +
                           reg_params_pos[params_offset_d];
                    diff = curData - pred;
                    itvNum = fabs(diff) / tmp_realPrecision + 1;
                    if (itvNum < intvCapacity) {
                      if (diff < 0)
                        itvNum = -itvNum;
                      type[index] = (int)(itvNum / 2) + intvRadius;
                      pred = pred +
                             2 * (type[index] - intvRadius) * tmp_realPrecision;
                      // ganrantee comporession error against the case of
                      // machine-epsilon
                      if (fabs(curData - pred) > tmp_realPrecision) {
                        type[index] = 0;
                        pred = curData;
                        unpredictable_data[block_unpredictable_count++] =
                          curData;
                      }
                    } else {
                      type[index] = 0;
                      pred = curData;
                      unpredictable_data[block_unpredictable_count++] = curData;
                    }
                    index++;
                    cur_data_pos++;
                  }
                  cur_data_pos++;
                }
                cur_data_pos += pred_buffer_block_size;
              }
              reg_params_pos++;
              total_unpred += block_unpredictable_count;
              unpredictable_data += block_unpredictable_count;
              *blockwise_unpred_count_pos = block_unpredictable_count;
            } else {
              // use SZ
              // SZ predication
              unpredictable_count = 0;
              float* cur_data_pos =
                pred_buffer + pred_buffer_block_size * pred_buffer_block_size +
                pred_buffer_block_size + 1;
              float curData;
              float pred3D;
              double itvNum, diff;
              size_t index = 0;
              for (size_t ii = 0; ii < block_size; ii++) {
                for (size_t jj = 0; jj < block_size; jj++) {
                  for (size_t kk = 0; kk < block_size; kk++) {

                    curData = *cur_data_pos;
                    if (fabs(curData - mean) <= realPrecision) {
                      type[index] = 1;
                      *cur_data_pos = mean;
                    } else {
                      pred3D =
                        cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                        cur_data_pos[-strip_dim0_offset] -
                        cur_data_pos[-strip_dim1_offset - 1] -
                        cur_data_pos[-strip_dim0_offset - 1] -
                        cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                        cur_data_pos[-strip_dim0_offset - strip_dim1_offset -
                                     1];
                      diff = curData - pred3D;
                      itvNum = fabs(diff) / realPrecision + 1;
                      if (itvNum < intvCapacity_sz) {
                        if (diff < 0)
                          itvNum = -itvNum;
                        type[index] = (int)(itvNum / 2) + intvRadius;
                        *cur_data_pos = pred3D + 2 *
                                                   (type[index] - intvRadius) *
                                                   tmp_realPrecision;
                        // ganrantee comporession error against the case of
                        // machine-epsilon
                        if (fabs(curData - *cur_data_pos) > tmp_realPrecision) {
                          type[index] = 0;
                          *cur_data_pos = curData;
                          unpredictable_data[unpredictable_count++] = curData;
                        }
                      } else {
                        type[index] = 0;
                        *cur_data_pos = curData;
                        unpredictable_data[unpredictable_count++] = curData;
                      }
                    }
                    index++;
                    cur_data_pos++;
                  }
                  cur_data_pos++;
                }
                cur_data_pos += pred_buffer_block_size;
              }
              total_unpred += unpredictable_count;
              unpredictable_data += unpredictable_count;
              *blockwise_unpred_count_pos = unpredictable_count;
            } // end SZ
            blockwise_unpred_count_pos++;
            type += block_size * block_size * block_size;
          } // end k
          indicator_pos += num_z;
        } // end j
      }   // end i
    } else {
      int intvCapacity_sz = intvCapacity - 2;
      type = result_type;
      for (size_t i = 0; i < num_x; i++) {
        for (size_t j = 0; j < num_y; j++) {
          for (size_t k = 0; k < num_z; k++) {
            data_pos = oriData + i * block_size * dim0_offset +
                       j * block_size * dim1_offset + k * block_size;
            // add 1 in x, y, z offset
            pred_buffer_pos = pred_buffer +
                              pred_buffer_block_size * pred_buffer_block_size +
                              pred_buffer_block_size + 1;
            block_data_pos_x = data_pos;
            for (size_t ii = 0; ii < block_size; ii++) {
              block_data_pos_y = block_data_pos_x;
              for (size_t jj = 0; jj < block_size; jj++) {
                block_data_pos_z = block_data_pos_y;
                for (size_t kk = 0; kk < block_size; kk++) {
                  *pred_buffer_pos = *block_data_pos_z;
                  if (k * block_size + kk < r3)
                    block_data_pos_z++;
                  pred_buffer_pos++;
                }
                // add 1 in z offset
                pred_buffer_pos++;
                if (j * block_size + jj < r2)
                  block_data_pos_y += dim1_offset;
              }
              // add 1 in y offset
              pred_buffer_pos += pred_buffer_block_size;
              if (i * block_size + ii < r1)
                block_data_pos_x += dim0_offset;
            }
            if (!(indicator_pos[k])) {
              float curData;
              float pred;
              double itvNum;
              double diff;
              size_t index = 0;
              size_t block_unpredictable_count = 0;
              float* cur_data_pos =
                pred_buffer + pred_buffer_block_size * pred_buffer_block_size +
                pred_buffer_block_size + 1;
              for (size_t ii = 0; ii < block_size; ii++) {
                for (size_t jj = 0; jj < block_size; jj++) {
                  for (size_t kk = 0; kk < block_size; kk++) {
                    curData = *cur_data_pos;
                    pred = reg_params_pos[0] * ii +
                           reg_params_pos[params_offset_b] * jj +
                           reg_params_pos[params_offset_c] * kk +
                           reg_params_pos[params_offset_d];
                    diff = curData - pred;
                    itvNum = fabs(diff) / tmp_realPrecision + 1;
                    if (itvNum < intvCapacity) {
                      if (diff < 0)
                        itvNum = -itvNum;
                      type[index] = (int)(itvNum / 2) + intvRadius;
                      pred = pred +
                             2 * (type[index] - intvRadius) * tmp_realPrecision;
                      // ganrantee comporession error against the case of
                      // machine-epsilon
                      if (fabs(curData - pred) > tmp_realPrecision) {
                        type[index] = 0;
                        pred = curData;
                        unpredictable_data[block_unpredictable_count++] =
                          curData;
                      }
                    } else {
                      type[index] = 0;
                      pred = curData;
                      unpredictable_data[block_unpredictable_count++] = curData;
                    }
                    index++;
                    cur_data_pos++;
                  }
                  cur_data_pos++;
                }
                cur_data_pos += pred_buffer_block_size;
              }
              reg_params_pos++;
              total_unpred += block_unpredictable_count;
              unpredictable_data += block_unpredictable_count;
              *blockwise_unpred_count_pos = block_unpredictable_count;
            } else {
              // use SZ
              // SZ predication
              unpredictable_count = 0;
              float* cur_data_pos =
                pred_buffer + pred_buffer_block_size * pred_buffer_block_size +
                pred_buffer_block_size + 1;
              float curData;
              float pred3D;
              double itvNum, diff;
              size_t index = 0;
              for (size_t ii = 0; ii < block_size; ii++) {
                for (size_t jj = 0; jj < block_size; jj++) {
                  for (size_t kk = 0; kk < block_size; kk++) {
                    curData = *cur_data_pos;
                    pred3D =
                      cur_data_pos[-1] + cur_data_pos[-strip_dim1_offset] +
                      cur_data_pos[-strip_dim0_offset] -
                      cur_data_pos[-strip_dim1_offset - 1] -
                      cur_data_pos[-strip_dim0_offset - 1] -
                      cur_data_pos[-strip_dim0_offset - strip_dim1_offset] +
                      cur_data_pos[-strip_dim0_offset - strip_dim1_offset - 1];
                    diff = curData - pred3D;
                    itvNum = fabs(diff) / realPrecision + 1;
                    if (itvNum < intvCapacity_sz) {
                      if (diff < 0)
                        itvNum = -itvNum;
                      type[index] = (int)(itvNum / 2) + intvRadius;
                      *cur_data_pos = pred3D + 2 * (type[index] - intvRadius) *
                                                 tmp_realPrecision;
                      // ganrantee comporession error against the case of
                      // machine-epsilon
                      if (fabs(curData - *cur_data_pos) > tmp_realPrecision) {
                        type[index] = 0;
                        *cur_data_pos = curData;
                        unpredictable_data[unpredictable_count++] = curData;
                      }
                    } else {
                      type[index] = 0;
                      *cur_data_pos = curData;
                      unpredictable_data[unpredictable_count++] = curData;
                    }
                    index++;
                    cur_data_pos++;
                  }
                  cur_data_pos++;
                }
                cur_data_pos += pred_buffer_block_size;
              }
              total_unpred += unpredictable_count;
              unpredictable_data += unpredictable_count;
              *blockwise_unpred_count_pos = unpredictable_count;
            } // end SZ
            blockwise_unpred_count_pos++;
            type += block_size * block_size * block_size;
          }
          indicator_pos += num_z;
        }
      }
    }

    free(pred_buffer);
    int stateNum = 2 * quantization_intervals;
    HuffmanTree* huffmanTree = createHuffmanTree(stateNum);

    size_t nodeCount = 0;
    init(huffmanTree, result_type, num_blocks * max_num_block_elements);
    int i = 0;
    for (i = 0; i < huffmanTree->stateNum; i++)
      if (huffmanTree->code[i])
        nodeCount++;
    nodeCount = nodeCount * 2 - 1;

    unsigned char* treeBytes;
    unsigned int treeByteSize =
      convert_HuffTree_to_bytes_anyStates(huffmanTree, nodeCount, &treeBytes);

    unsigned int meta_data_offset = 3 + 1 + MetaDataByteLength;
    // total size 										metadata		  # elements     real precision		intervals
    // nodeCount		huffman 	 	block index 						unpredicatable count						mean
    // unpred size 				elements
    unsigned char* result = (unsigned char*)calloc(
      meta_data_offset + exe_params->SZ_SIZE_TYPE + sizeof(double) +
        sizeof(int) + sizeof(int) + treeByteSize +
        num_blocks * sizeof(unsigned short) +
        num_blocks * sizeof(unsigned short) + num_blocks * sizeof(float) +
        total_unpred * sizeof(float) + num_elements * sizeof(int),
      1);
    unsigned char* result_pos = result;
    initRandomAccessBytes(result_pos);

    result_pos += meta_data_offset;

    sizeToBytes(result_pos, num_elements); // SZ_SIZE_TYPE: 4 or 8
    result_pos += exe_params->SZ_SIZE_TYPE;

    intToBytes_bigEndian(result_pos, block_size);
    result_pos += sizeof(int);
    doubleToBytes(result_pos, realPrecision);
    result_pos += sizeof(double);
    intToBytes_bigEndian(result_pos, quantization_intervals);
    result_pos += sizeof(int);
    intToBytes_bigEndian(result_pos, treeByteSize);
    result_pos += sizeof(int);
    intToBytes_bigEndian(result_pos, nodeCount);
    result_pos += sizeof(int);
    memcpy(result_pos, treeBytes, treeByteSize);
    result_pos += treeByteSize;
    free(treeBytes);

    memcpy(result_pos, &use_mean, sizeof(unsigned char));
    result_pos += sizeof(unsigned char);
    memcpy(result_pos, &mean, sizeof(float));
    result_pos += sizeof(float);
    size_t indicator_size = convertIntArray2ByteArray_fast_1b_to_result(
      indicator, num_blocks, result_pos);
    result_pos += indicator_size;

    // convert the lead/mid/resi to byte stream
    if (reg_count > 0) {
      for (int e = 0; e < 4; e++) {
        int stateNum = 2 * coeff_intvCapacity_sz;
        HuffmanTree* huffmanTree = createHuffmanTree(stateNum);
        size_t nodeCount = 0;
        init(huffmanTree, coeff_type[e], reg_count);
        int i = 0;
        for (i = 0; i < huffmanTree->stateNum; i++)
          if (huffmanTree->code[i])
            nodeCount++;
        nodeCount = nodeCount * 2 - 1;
        unsigned char* treeBytes;
        unsigned int treeByteSize = convert_HuffTree_to_bytes_anyStates(
          huffmanTree, nodeCount, &treeBytes);
        doubleToBytes(result_pos, precision[e]);
        result_pos += sizeof(double);
        intToBytes_bigEndian(result_pos, coeff_intvRadius);
        result_pos += sizeof(int);
        intToBytes_bigEndian(result_pos, treeByteSize);
        result_pos += sizeof(int);
        intToBytes_bigEndian(result_pos, nodeCount);
        result_pos += sizeof(int);
        memcpy(result_pos, treeBytes, treeByteSize);
        result_pos += treeByteSize;
        free(treeBytes);
        size_t typeArray_size = 0;
        encode(huffmanTree, coeff_type[e], reg_count,
               result_pos + sizeof(size_t), &typeArray_size);
        sizeToBytes(result_pos, typeArray_size);
        result_pos += sizeof(size_t) + typeArray_size;
        intToBytes_bigEndian(result_pos, coeff_unpredictable_count[e]);
        result_pos += sizeof(int);
        memcpy(result_pos, coeff_unpred_data[e],
               coeff_unpredictable_count[e] * sizeof(float));
        result_pos += coeff_unpredictable_count[e] * sizeof(float);
        SZ_ReleaseHuffman(huffmanTree);
      }
    }
    free(coeff_result_type);
    free(coeff_unpredictable_data);

    // record the number of unpredictable data and also store them
    memcpy(result_pos, &total_unpred, sizeof(size_t));
    result_pos += sizeof(size_t);
    // record blockwise unpred data
    size_t compressed_blockwise_unpred_count_size;
    unsigned char* compressed_bw_unpred_count = SZ_compress_args(
      SZ_INT32, blockwise_unpred_count, &compressed_blockwise_unpred_count_size,
      ABS, 0.5, 0, 0, 0, 0, 0, 0, num_blocks);
    memcpy(result_pos, &compressed_blockwise_unpred_count_size, sizeof(size_t));
    result_pos += sizeof(size_t);
    memcpy(result_pos, compressed_bw_unpred_count,
           compressed_blockwise_unpred_count_size);
    result_pos += compressed_blockwise_unpred_count_size;
    free(blockwise_unpred_count);
    free(compressed_bw_unpred_count);
    memcpy(result_pos, result_unpredictable_data, total_unpred * sizeof(float));
    result_pos += total_unpred * sizeof(float);

    free(reg_params);
    free(indicator);
    free(result_unpredictable_data);
    // encode type array by block
    type = result_type;
    size_t total_type_array_size = 0;
    unsigned char* type_array_buffer =
      (unsigned char*)malloc(num_blocks * max_num_block_elements * sizeof(int));
    unsigned short* type_array_block_size =
      (unsigned short*)malloc(num_blocks * sizeof(unsigned short));
    unsigned char* type_array_buffer_pos = type_array_buffer;
    unsigned short* type_array_block_size_pos = type_array_block_size;
    for (size_t i = 0; i < num_x; i++) {
      for (size_t j = 0; j < num_y; j++) {
        for (size_t k = 0; k < num_z; k++) {
          size_t typeArray_size = 0;
          encode(huffmanTree, type, max_num_block_elements,
                 type_array_buffer_pos, &typeArray_size);
          total_type_array_size += typeArray_size;
          *type_array_block_size_pos = typeArray_size;
          type_array_buffer_pos += typeArray_size;
          type += max_num_block_elements;
          type_array_block_size_pos++;
        }
      }
    }
    size_t compressed_type_array_block_size;
    unsigned char* compressed_type_array_block = SZ_compress_args(
      SZ_UINT16, type_array_block_size, &compressed_type_array_block_size, ABS,
      0.5, 0, 0, 0, 0, 0, 0, num_blocks);
    memcpy(result_pos, &compressed_type_array_block_size, sizeof(size_t));
    result_pos += sizeof(size_t);
    memcpy(result_pos, compressed_type_array_block,
           compressed_type_array_block_size);
    result_pos += compressed_type_array_block_size;
    memcpy(result_pos, type_array_buffer, total_type_array_size);
    result_pos += total_type_array_size;
    // size_t typeArray_size = 0;
    // encode(huffmanTree, result_type, num_blocks*max_num_block_elements,
    // result_pos, &typeArray_size); result_pos += typeArray_size;

    free(type_array_buffer);
    free(type_array_block_size);
    size_t totalEncodeSize = result_pos - result;
    free(result_type);
    SZ_ReleaseHuffman(huffmanTree);
    *comp_size = totalEncodeSize;
    return result;
  }
}
