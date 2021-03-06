/*
 * Copyright (c) Imazen LLC.
 * No part of this project, including this file, may be copied, modified,
 * propagated, or distributed except as permitted in COPYRIGHT.txt.
 * Licensed under the GNU Affero General Public License, Version 3.0.
 * Commercial licenses available at http://imageresizing.net/
 */
#ifdef _MSC_VER
#pragma unmanaged
#endif

#include "imageflow_private.h"

#include <string.h>

#ifndef _MSC_VER
#include <alloca.h>
#else
#pragma unmanaged
#ifndef alloca
#include <malloc.h>
#define alloca _alloca
#endif
#endif

struct flow_convolution_kernel * flow_convolution_kernel_create(flow_c * context, uint32_t radius)
{
    struct flow_convolution_kernel * k = FLOW_calloc_array(context, 1, struct flow_convolution_kernel);
    // For the actual array;
    float * a = FLOW_calloc_array(context, radius * 2 + 1, float);
    // we assume a maximum of 4 channels are going to need buffering during convolution
    float * buf = (float *)FLOW_malloc(context, (radius + 2) * 4 * sizeof(float));

    if (k == NULL || a == NULL || buf == NULL) {
        FLOW_free(context, k);
        FLOW_free(context, a);
        FLOW_free(context, buf);
        FLOW_error(context, flow_status_Out_of_memory);
        return NULL;
    }
    k->kernel = a;
    k->width = radius * 2 + 1;
    k->buffer = buf;
    k->radius = radius;
    return k;
}
void flow_convolution_kernel_destroy(flow_c * context, struct flow_convolution_kernel * kernel)
{
    if (kernel != NULL) {
        FLOW_free(context, kernel->kernel);
        FLOW_free(context, kernel->buffer);
        kernel->kernel = NULL;
        kernel->buffer = NULL;
    }
    FLOW_free(context, kernel);
}

struct flow_convolution_kernel * flow_convolution_kernel_create_guassian(flow_c * context, double stdDev,
                                                                         uint32_t radius)
{
    struct flow_convolution_kernel * k = flow_convolution_kernel_create(context, radius);
    if (k != NULL) {
        for (uint32_t i = 0; i < k->width; i++) {

            k->kernel[i] = (float)ir_guassian(abs((int)radius - (int)i), stdDev);
        }
    }
    return k;
}

double flow_convolution_kernel_sum(struct flow_convolution_kernel * kernel)
{
    double sum = 0;
    for (uint32_t i = 0; i < kernel->width; i++) {
        sum += kernel->kernel[i];
    }
    return sum;
}

void flow_convolution_kernel_normalize(struct flow_convolution_kernel * kernel, float desiredSum)
{
    double sum = flow_convolution_kernel_sum(kernel);
    if (sum == 0)
        return; // nothing to do here, zeroes are as normalized as you can get ;)
    float factor = (float)(desiredSum / sum);
    for (uint32_t i = 0; i < kernel->width; i++) {
        kernel->kernel[i] *= factor;
    }
}
struct flow_convolution_kernel * flow_convolution_kernel_create_gaussian_normalized(flow_c * context, double stdDev,
                                                                                    uint32_t radius)
{
    struct flow_convolution_kernel * kernel = flow_convolution_kernel_create_guassian(context, stdDev, radius);
    if (kernel != NULL) {
        flow_convolution_kernel_normalize(kernel, 1);
    }
    return kernel;
}

struct flow_convolution_kernel * flow_convolution_kernel_create_guassian_sharpen(flow_c * context, double stdDev,
                                                                                 uint32_t radius)
{
    struct flow_convolution_kernel * kernel = flow_convolution_kernel_create_guassian(context, stdDev, radius);
    if (kernel != NULL) {
        double sum = flow_convolution_kernel_sum(kernel);
        for (uint32_t i = 0; i < kernel->width; i++) {
            if (i == radius) {
                kernel->kernel[i] = (float)(2 * sum - kernel->kernel[i]);
            } else {
                kernel->kernel[i] *= -1;
            }
        }
        flow_convolution_kernel_normalize(kernel, 1);
    }
    return kernel;
}

bool flow_bitmap_float_convolve_rows(flow_c * context, struct flow_bitmap_float * buf,
                                     struct flow_convolution_kernel * kernel, uint32_t convolve_channels,
                                     uint32_t from_row, int row_count)
{

    const uint32_t radius = kernel->radius;
    const float threshold_min = kernel->threshold_min_change;
    const float threshold_max = kernel->threshold_max_change;

    // Do nothing unless the image is at least half as wide as the kernel.
    if (buf->w < radius + 1)
        return true;

    const uint32_t buffer_count = radius + 1;
    const uint32_t w = buf->w;
    const int32_t int_w = (int32_t)buf->w;
    const uint32_t step = buf->channels;

    const uint32_t until_row = row_count < 0 ? buf->h : from_row + (unsigned)row_count;

    const uint32_t ch_used = convolve_channels;

    float * __restrict buffer = kernel->buffer;
    float * __restrict avg = &kernel->buffer[buffer_count * ch_used];

    const float * __restrict kern = kernel->kernel;

    const int wrap_mode = 0;

    for (uint32_t row = from_row; row < until_row; row++) {

        float * __restrict source_buffer = &buf->pixels[row * buf->float_stride];
        int circular_idx = 0;

        for (uint32_t ndx = 0; ndx < w + buffer_count; ndx++) {
            // Flush old value
            if (ndx >= buffer_count) {
                memcpy(&source_buffer[(ndx - buffer_count) * step], &buffer[circular_idx * ch_used],
                       ch_used * sizeof(float));
            }
            // Calculate and enqueue new value
            if (ndx < w) {
                const int left = ndx - radius;
                const int right = ndx + radius;
                int i;

                memset(avg, 0, sizeof(float) * ch_used);

                if (left < 0 || right >= (int32_t)w) {
                    if (wrap_mode == 0) {
                        // Only sample what's present, and fix the average later.
                        float total_weight = 0;
                        /* Accumulate each channel */
                        for (i = left; i <= right; i++) {
                            if (i > 0 && i < int_w) {
                                const float weight = kern[i - left];
                                total_weight += weight;
                                for (uint32_t j = 0; j < ch_used; j++)
                                    avg[j] += weight * source_buffer[i * step + j];
                            }
                        }
                        for (uint32_t j = 0; j < ch_used; j++)
                            avg[j] = avg[j] / total_weight;
                    } else if (wrap_mode == 1) {
                        // Extend last pixel to be used for all missing inputs
                        /* Accumulate each channel */
                        for (i = left; i <= right; i++) {
                            const float weight = kern[i - left];
                            const uint32_t ix = EVIL_CLAMP(i, 0, int_w - 1);
                            for (uint32_t j = 0; j < ch_used; j++)
                                avg[j] += weight * source_buffer[ix * step + j];
                        }
                    }
                } else {
                    /* Accumulate each channel */
                    for (i = left; i <= right; i++) {
                        const float weight = kern[i - left];
                        for (uint32_t j = 0; j < ch_used; j++)
                            avg[j] += weight * source_buffer[i * step + j];
                    }
                }

                // Enqueue difference
                memcpy(&buffer[circular_idx * ch_used], avg, ch_used * sizeof(float));

                if (threshold_min > 0 || threshold_max > 0) {
                    float change = 0;
                    for (uint32_t j = 0; j < ch_used; j++)
                        change += (float)fabs(source_buffer[ndx * step + j] - avg[j]);

                    if (change < threshold_min || change > threshold_max) {
                        memcpy(&buffer[circular_idx * ch_used], &source_buffer[ndx * step], ch_used * sizeof(float));
                    }
                }
            }
            circular_idx = (circular_idx + 1) % buffer_count;
        }
    }
    return true;
}

static bool BitmapFloat_boxblur_rows(flow_c * context, struct flow_bitmap_float * image, uint32_t radius,
                                     uint32_t passes, const uint32_t convolve_channels, float * work_buffer,
                                     uint32_t from_row, int row_count)
{
    const uint32_t buffer_count = radius + 1;
    const uint32_t w = image->w;
    const uint32_t step = image->channels;
    const uint32_t until_row = row_count < 0 ? image->h : from_row + (unsigned)row_count;
    const uint32_t ch_used = image->channels;
    float * __restrict buffer = work_buffer;
    const uint32_t std_count = radius * 2 + 1;
    const float std_factor = 1.0f / (float)(std_count);
    for (uint32_t row = from_row; row < until_row; row++) {
        float * __restrict source_buffer = &image->pixels[row * image->float_stride];
        for (uint32_t pass_index = 0; pass_index < passes; pass_index++) {
            int circular_idx = 0;
            float sum[4] = { 0, 0, 0, 0 };
            uint32_t count = 0;
            for (uint32_t ndx = 0; ndx < radius; ndx++) {
                for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                    sum[ch] += source_buffer[ndx * step + ch];
                }
                count++;
            }
            for (uint32_t ndx = 0; ndx < w + buffer_count; ndx++) { // Pixels
                if (ndx >= buffer_count) { // same as ndx > radius
                    // Remove trailing item from average
                    for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                        sum[ch] -= source_buffer[(ndx - radius - 1) * step + ch];
                    }
                    count--;
                    // Flush old value
                    memcpy(&source_buffer[(ndx - buffer_count) * step], &buffer[circular_idx * ch_used],
                           ch_used * sizeof(float));
                }
                // Calculate and enqueue new value
                if (ndx < w) {
                    if (ndx < w - radius) {
                        for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                            sum[ch] += source_buffer[(ndx + radius) * step + ch];
                        }
                        count++;
                    }
                    // Enqueue averaged value
                    if (count != std_count) {
                        for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                            buffer[circular_idx * ch_used + ch] = sum[ch] / (float)count; // Recompute factor
                        }
                    } else {
                        for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                            buffer[circular_idx * ch_used + ch] = sum[ch] * std_factor;
                        }
                    }
                }
                circular_idx = (circular_idx + 1) % buffer_count;
            }
        }
    }
    return true;
}
static bool BitmapFloat_boxblur_misaligned_rows(flow_c * context, struct flow_bitmap_float * image, uint32_t radius,
                                                int align, const uint32_t convolve_channels, float * work_buffer,
                                                uint32_t from_row, int row_count)
{
    if (align != 1 && align != -1) {
        FLOW_error(context, flow_status_Invalid_internal_state);
        return false;
    }
    const uint32_t buffer_count = radius + 2;
    const uint32_t w = image->w;
    const uint32_t step = image->channels;
    const uint32_t until_row = row_count < 0 ? image->h : from_row + (unsigned)row_count;
    const uint32_t ch_used = image->channels;
    float * __restrict buffer = work_buffer;
    const uint32_t write_offset = align == -1 ? 0 : 1;
    for (uint32_t row = from_row; row < until_row; row++) {
        float * __restrict source_buffer = &image->pixels[row * image->float_stride];
        int circular_idx = 0;
        float sum[4] = { 0, 0, 0, 0 };
        float count = 0;
        for (uint32_t ndx = 0; ndx < radius; ndx++) {
            float factor = (ndx == radius - 1) ? 0.5f : 1;
            for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                sum[ch] += source_buffer[ndx * step + ch] * factor;
            }
            count += factor;
        }
        for (uint32_t ndx = 0; ndx < w + buffer_count - write_offset; ndx++) { // Pixels
            // Calculate new value
            if (ndx < w) {
                if (ndx < w - radius) {
                    for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                        sum[ch] += source_buffer[(ndx + radius) * step + ch] * 0.5f;
                    }
                    count += 0.5f;
                }
                if (ndx < w - radius + 1) {
                    for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                        sum[ch] += source_buffer[(ndx - 1 + radius) * step + ch] * 0.5f;
                    }
                    count += 0.5f;
                }
                // Remove trailing items from average
                if (ndx >= radius) {
                    for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                        sum[ch] -= source_buffer[(ndx - radius) * step + ch] * 0.5f;
                    }
                    count -= 0.5f;
                }
                if (ndx >= radius + 1) {
                    for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                        sum[ch] -= source_buffer[(ndx - 1 - radius) * step + ch] * 0.5f;
                    }
                    count -= 0.5f;
                }
            }
            // Flush old value
            if (ndx >= buffer_count - write_offset) {
                memcpy(&source_buffer[(ndx + write_offset - buffer_count) * step], &buffer[circular_idx * ch_used],
                       ch_used * sizeof(float));
            }
            // enqueue new value
            if (ndx < w) {
                for (uint32_t ch = 0; ch < convolve_channels; ch++) {
                    buffer[circular_idx * ch_used + ch] = sum[ch] / (float)count;
                }
            }
            circular_idx = (circular_idx + 1) % buffer_count;
        }
    }
    return true;
}

uint32_t flow_bitmap_float_approx_gaussian_calculate_d(float sigma, uint32_t bitmap_width)
{
    uint32_t d = (int)floorf(1.8799712059732503768118239636082839397552400554574537f * sigma + 0.5f);
    d = umin(d, (bitmap_width - 1) / 2); // Never exceed half the size of the buffer.
    return d;
}

uint32_t flow_bitmap_float_approx_gaussian_buffer_element_count_required(float sigma, uint32_t bitmap_width)
{
    return flow_bitmap_float_approx_gaussian_calculate_d(sigma, bitmap_width) * 2 + 12; // * sizeof(float);
}
bool flow_bitmap_float_approx_gaussian_blur_rows(flow_c * context, struct flow_bitmap_float * image, float sigma,
                                                 float * buffer, size_t buffer_element_count, uint32_t from_row,
                                                 int row_count)
{
    // Ensure sigma is large enough for approximation to be accurate.
    if (sigma < 2) {
        FLOW_error(context, flow_status_Invalid_internal_state);
        return false;
    }

    // Ensure the buffer is large enough
    if (flow_bitmap_float_approx_gaussian_buffer_element_count_required(sigma, image->w) > buffer_element_count) {
        FLOW_error(context, flow_status_Invalid_internal_state);
        return false;
    }

    // http://www.w3.org/TR/SVG11/filters.html#feGaussianBlur
    // For larger values of 's' (s >= 2.0), an approximation can be used :
    // Three successive box - blurs build a piece - wise quadratic convolution kernel, which approximates the Gaussian
    // kernel to within roughly 3 % .
    uint32_t d = flow_bitmap_float_approx_gaussian_calculate_d(sigma, image->w);
    //... if d is odd, use three box - blurs of size 'd', centered on the output pixel.
    if (d % 2 > 0) {
        if (!BitmapFloat_boxblur_rows(context, image, d / 2, 3, image->channels, buffer, from_row, row_count)) {
            FLOW_error_return(context);
        }
    } else {
        // ... if d is even, two box - blurs of size 'd'
        // (the first one centered on the pixel boundary between the output pixel and the one to the left,
        //  the second one centered on the pixel boundary between the output pixel and the one to the right)
        // and one box blur of size 'd+1' centered on the output pixel.
        if (!BitmapFloat_boxblur_misaligned_rows(context, image, d / 2, -1, image->channels, buffer, from_row,
                                                 row_count)) {
            FLOW_error_return(context);
        }
        if (!BitmapFloat_boxblur_misaligned_rows(context, image, d / 2, 1, image->channels, buffer, from_row,
                                                 row_count)) {
            FLOW_error_return(context);
        }
        if (!BitmapFloat_boxblur_rows(context, image, d / 2 + 1, 1, image->channels, buffer, from_row, row_count)) {
            FLOW_error_return(context);
        }
    }
    return true;
}

// static void flow_bitmap_bgra_sharpen_in_place_x(struct flow_bitmap_bgra * im, float pct)
//{
//    const float n = (float)(-pct / (pct - 1)); //if 0 < pct < 1
//    const float c_o = n / -2.0f;
//    const float c_i = n + 1;
//
//    uint32_t y, current, next;
//
//    const uint32_t sy = im->h;
//    const uint32_t stride = im->stride;
//    const uint32_t bytes_pp = flow_pixel_format_bytes_per_pixel (im->fmt);
//
//
//    if (pct <= 0 || im->w < 3 || bytes_pp < 3) return;
//
//    for (y = 0; y < sy; y++)
//    {
//
//        unsigned char *row = im->pixels + y * stride;
//        float left_b = (float)row[0];
//        float left_g = (float)row[1];
//        float left_r = (float)row[2];
//        for (current = bytes_pp, next = bytes_pp + bytes_pp; next < stride; current = next,
// next += bytes_pp){
//            const float b = row[current + 0];
//            const float g = row[current + 1];
//            const float r = row[current + 2];
//            row[current + 0] = left_b * c_o + b * c_i + row[current + bytes_pp + 0] * c_o;
//            row[current + 1] = left_g * c_o + g * c_i + row[current + bytes_pp + 1] * c_o;
//            row[current + 2] = left_r * c_o + r * c_i + row[current + bytes_pp + 2] * c_o;
//            left_b = b;
//            left_g = g;
//            left_r = r;
//        }
//    }
//}

static void flow_bitmap_bgra32_sharpen_block_edges_x(struct flow_bitmap_bgra * im, int block_size, float pct)
{
    pct = pct / 100.0f;
    if (pct < -1.0f)
        pct = -1;
    if (pct > 1.0f)
        pct = 1;
    const float n = (float)(-pct / (pct - 1.0)); // if 0 < pct < 1
    // indexes used are coord % block_size
    float c_l[7]; // left coefficient
    float c_c[7]; // center
    float c_r[7]; // right
    for (int i = 0; i < block_size; i++) {
        c_c[i] = 1;
        c_l[i] = 0;
        c_r[i] = 0;
    }
    if (block_size == 1) {
        c_c[0] = n + 1;
        c_r[0] = c_l[0] = n / -2.0f;
    } else {
        // Only adjust edge pixels for each block;
        c_c[0] = c_c[block_size - 1] = n / 2.0f + 1;
        c_l[0] = c_r[block_size - 1] = n / -2.0f;
    }

    uint32_t y, current, next;
    const uint32_t sy = im->h;
    const uint32_t stride = im->stride;
    const uint32_t bytes_pp = 4;

    if (im->w < 3 || flow_pixel_format_bytes_per_pixel(im->fmt) != 4)
        return;

    for (y = 0; y < sy; y++) {
        unsigned char * row = im->pixels + y * stride;
        float left_b = (float)row[0];
        float left_g = (float)row[1];
        float left_r = (float)row[2];
        int coord = 0;
        for (current = bytes_pp, next = bytes_pp + bytes_pp; next < stride; current = next, next += bytes_pp) {
            const float b = row[current + 0];
            const float g = row[current + 1];
            const float r = row[current + 2];
            const int weight_ix = coord % block_size;
            row[current + 0] = uchar_clamp_ff(left_b * c_l[weight_ix] + b * c_c[weight_ix]
                                              + row[current + bytes_pp + 0] * c_r[weight_ix]);
            row[current + 1] = uchar_clamp_ff(left_g * c_l[weight_ix] + g * c_c[weight_ix]
                                              + row[current + bytes_pp + 1] * c_r[weight_ix]);
            row[current + 2] = uchar_clamp_ff(left_r * c_l[weight_ix] + r * c_c[weight_ix]
                                              + row[current + bytes_pp + 2] * c_r[weight_ix]);
            left_b = b;
            left_g = g;
            left_r = r;
            coord++;
        }
    }
}
FLOW_HINT_HOT FLOW_HINT_UNSAFE_MATH_OPTIMIZATIONS

    bool
    flow_bitmap_bgra_transpose(flow_c * c, struct flow_bitmap_bgra * from, struct flow_bitmap_bgra * to)
{
    if (from->w != to->h || from->h != to->w || from->fmt != to->fmt || from->fmt != flow_bgra32) {
        FLOW_error(c, flow_status_Invalid_argument);
        return false;
    }
    int step = flow_pixel_format_bytes_per_pixel(to->fmt);
    for (uint32_t x = 0; x < to->w; x++) {
        for (uint32_t y = 0; y < to->h; y++) {
            *((uint32_t *)&to->pixels[x * step + y * to->stride])
                = *((uint32_t *)&from->pixels[x * from->stride + y * step]);
        }
    }
    return true;
}
FLOW_HINT_HOT FLOW_HINT_UNSAFE_MATH_OPTIMIZATIONS

    bool
    flow_bitmap_bgra_sharpen_block_edges(flow_c * c, struct flow_bitmap_bgra * im, int block_size, float pct)
{
    if (pct == 0.0f)
        return true;
    if (im->fmt != flow_bgra32) {
        FLOW_error(c, flow_status_Unsupported_pixel_format);
        return false;
    }
    flow_bitmap_bgra32_sharpen_block_edges_x(im, block_size, pct);
    struct flow_bitmap_bgra * temp = flow_bitmap_bgra_create(c, im->h, im->w, false, im->fmt);
    if (temp == NULL) {
        FLOW_add_to_callstack(c);
        return false;
    }
    if (!flow_bitmap_bgra_transpose(c, im, temp)) {
        flow_bitmap_bgra_destroy(c, temp);
        FLOW_add_to_callstack(c);
        return false;
    }
    flow_bitmap_bgra32_sharpen_block_edges_x(temp, block_size, pct);
    if (!flow_bitmap_bgra_transpose(c, temp, im)) {
        flow_bitmap_bgra_destroy(c, temp);
        FLOW_add_to_callstack(c);
        return false;
    }
    flow_bitmap_bgra_destroy(c, temp);
    return true;
}
FLOW_HINT_HOT

static void SharpenBgraFloatInPlace(float * buf, unsigned int count, double pct, int step)
{

    const float n = (float)(-pct / (pct - 1)); // if 0 < pct < 1
    const float c_o = n / -2.0f;
    const float c_i = n + 1;

    unsigned int ndx;

    // if both have alpha, process it
    if (step == 4) {
        float left_b = buf[0 * 4 + 0];
        float left_g = buf[0 * 4 + 1];
        float left_r = buf[0 * 4 + 2];
        float left_a = buf[0 * 4 + 3];

        for (ndx = 1; ndx < count - 1; ndx++) {
            const float b = buf[ndx * 4 + 0];
            const float g = buf[ndx * 4 + 1];
            const float r = buf[ndx * 4 + 2];
            const float a = buf[ndx * 4 + 3];
            buf[ndx * 4 + 0] = left_b * c_o + b * c_i + buf[(ndx + 1) * 4 + 0] * c_o;
            buf[ndx * 4 + 1] = left_g * c_o + g * c_i + buf[(ndx + 1) * 4 + 1] * c_o;
            buf[ndx * 4 + 2] = left_r * c_o + r * c_i + buf[(ndx + 1) * 4 + 2] * c_o;
            buf[ndx * 4 + 3] = left_a * c_o + a * c_i + buf[(ndx + 1) * 4 + 3] * c_o;
            left_b = b;
            left_g = g;
            left_r = r;
            left_a = a;
        }
    }
    // otherwise do the same thing without 4th chan
    // (ifs in loops are expensive..)
    else {
        float left_b = buf[0 * 3 + 0];
        float left_g = buf[0 * 3 + 1];
        float left_r = buf[0 * 3 + 2];

        for (ndx = 1; ndx < count - 1; ndx++) {
            const float b = buf[ndx * 3 + 0];
            const float g = buf[ndx * 3 + 1];
            const float r = buf[ndx * 3 + 2];
            buf[ndx * 3 + 0] = left_b * c_o + b * c_i + buf[(ndx + 1) * 3 + 0] * c_o;
            buf[ndx * 3 + 1] = left_g * c_o + g * c_i + buf[(ndx + 1) * 3 + 1] * c_o;
            buf[ndx * 3 + 2] = left_r * c_o + r * c_i + buf[(ndx + 1) * 3 + 2] * c_o;
            left_b = b;
            left_g = g;
            left_r = r;
        }
    }
}

bool flow_bitmap_float_sharpen_rows(flow_c * context, struct flow_bitmap_float * im, uint32_t start_row,
                                    uint32_t row_count, double pct)
{
    if (!(start_row + row_count <= im->h)) {
        FLOW_error(context, flow_status_Invalid_internal_state);
        return false;
    }
    for (uint32_t row = start_row; row < start_row + row_count; row++) {
        SharpenBgraFloatInPlace(im->pixels + (im->float_stride * row), im->w, pct, im->channels);
    }
    return true;
}
