/* PS5 OpenGL v1.0.2 excerpts. Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Full ps5_screen.c SHA256: 6af0a6fc275afb2796563d9ecc8a4d25de93929548a9fadbd0e230e35b1d946f
 * Actual structs/functions and initialization paths; not a standalone TU. */
struct ps5_resource {
   struct pipe_resource base;
   uint64_t texture_publication_epoch, stencil_publication_epoch;
   size_t texture_published_bytes, stencil_published_bytes;
   bool external_cpu_access;
   bool gpu_written; /* CPU-cached lines may predate a compute write */
   struct pipe_resource *render_pool_owner;
   struct pipe_resource *stencil_sample;
   uint8_t *data;
   uint8_t *stencil_data;
   size_t size;
   size_t allocation_size;
   size_t stencil_allocation_size;
   unsigned stride;
   size_t level_offset[PIPE_MAX_TEXTURE_LEVELS];
   unsigned level_stride[PIPE_MAX_TEXTURE_LEVELS];
   size_t layer_stride;
   size_t render_staging_offset;
   size_t render_staging_size;
   size_t depth_staging_offset;
   size_t depth_staging_size;
   int64_t direct_start;
   int64_t stencil_direct_start;
   unsigned render_arena_first_slot;
   unsigned render_arena_slot_count;
};

struct ps5_transfer {
   struct pipe_transfer base;
   void *staging;
   size_t staging_mapping_size;
};

      memset(resource->data, 0, size);
#ifdef AGC_RUNTIME_DIAGNOSTICS
#endif
   memset(resource->data, 0, allocation_size);
primary_ready:
   if (templ->format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {
   resource->depth_staging_size = depth_staging_size;
   if (PS5_ENABLE_SHARED_RENDER_POOL_CANDIDATE &&

static void
ps5_transfer_free_staging(struct ps5_transfer *transfer)
{
   if (transfer->staging_mapping_size) {
#ifdef PS5_NATIVE_TITLE_RUNTIME
      if (!ps5_staging_put(transfer->staging, transfer->staging_mapping_size))
#endif
      if (munmap(transfer->staging, transfer->staging_mapping_size))
         fprintf(stderr, "[ps5-gallium] transfer staging munmap failed\n");
   } else {
      free(transfer->staging);
   }
   transfer->staging = NULL;
   transfer->staging_mapping_size = 0;
}

static void *
ps5_transfer_map(struct pipe_context *context, struct pipe_resource *base,
                 unsigned level, unsigned usage, const struct pipe_box *box,
                 struct pipe_transfer **out_transfer)
{
#if defined(PS5_NATIVE_TITLE_RUNTIME) && defined(PS5_DRAW_PROFILE)
   ps5_map_caller_record((uintptr_t)__builtin_return_address(0), usage,
                         base ? base->target : UINT_MAX);
   struct ps5_prepare_scope scope __attribute__((cleanup(ps5_prepare_scope_end))) =
      {6, ps5_prepare_clock()};
#endif

   struct ps5_resource *resource = (struct ps5_resource *)base;
   struct ps5_transfer *transfer;
   size_t offset;
   unsigned format_size;

   (void)context;
   if (!out_transfer)
      return NULL;
   *out_transfer = NULL;
   /* Mesa initializes incomplete multisample fallback textures through a
    * write map. Broadcast each supplied texel to all samples on unmap;
    * reads still require the existing explicit resolve path. */
   struct ps5_resource single_sample;
   const struct ps5_resource *bounds_resource = resource;
   if (base->nr_samples > 1) {
      if (base->nr_samples != 4 || base->nr_storage_samples != 4 ||
          !(usage & PIPE_MAP_WRITE) || (usage & PIPE_MAP_READ) ||
          !(base->bind & PIPE_BIND_RENDER_TARGET) ||
          !ps5_msaa4_color_format(base->format) ||
          (base->target != PIPE_TEXTURE_2D && base->target != PIPE_TEXTURE_2D_ARRAY))
         return NULL;
      single_sample = *resource;
      single_sample.base.nr_samples = single_sample.base.nr_storage_samples = 1;
      bounds_resource = &single_sample;
   }
   if (!ps5_map_bounds(bounds_resource, level, box, &offset))
      return NULL;
#if defined(PS5_NATIVE_TITLE_RUNTIME) && defined(PS5_DRAW_PROFILE)
   static unsigned trace_count;
   if (__atomic_load_n(&ps5_prepare_calls[0], __ATOMIC_RELAXED) >= 60000 &&
       __atomic_load_n(&trace_count, __ATOMIC_RELAXED) < 32 &&
       __atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED) < 32) {
      printf("[ps5-map-layout] target=%u format=%u bind=%x usage=%x level=%u "
             "image=%ux%u layers=%u box=%d,%d,%d/%dx%dx%d bytes=%zu\n",
             base->target, base->format, base->bind, usage, level,
             base->width0, base->height0, base->array_size,
             box->x, box->y, box->z, box->width, box->height, box->depth,
             resource->allocation_size);
   }
#endif
   /* Mesa's streaming uploader uses disjoint ranges in an existing buffer.
    * Honor its explicit no-wait contract; reads and texture staging still wait. */
   if (base->target != PIPE_BUFFER || (usage & PIPE_MAP_READ) ||
       !(usage & PIPE_MAP_UNSYNCHRONIZED))
      ps5_draw_batch_drain_buffer(base);
   else
      ps5_invalidate_gpu_writes(resource);

   if (usage & PIPE_MAP_PERSISTENT)
      resource->external_cpu_access = true;

   transfer = calloc(1, sizeof(*transfer));
   if (!transfer)
      return NULL;

   transfer->base.resource = base;
   transfer->base.level = level;
   transfer->base.usage = usage;
   transfer->base.box = *box;
   transfer->base.stride = resource->level_stride[level];
   transfer->base.layer_stride = resource->layer_stride;
   transfer->base.offset = (unsigned)offset;
   *out_transfer = &transfer->base;

   if ((usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) &&
       (resource->base.bind & PIPE_BIND_DEPTH_STENCIL) &&
       !resource->depth_staging_size &&
       (resource->base.format == PIPE_FORMAT_Z32_FLOAT ||
        resource->base.format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT)) {
      const bool packed =
         resource->base.format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
      const size_t depth_layer_size = ps5_tiled_depth_surface_size(
         resource->base.width0, resource->base.height0, 1);
      const size_t stencil_layer_size = ps5_tiled_stencil_surface_size(
         resource->base.width0, resource->base.height0);
      size_t staging_stride;
      size_t staging_layer_stride;
      size_t staging_size;

      format_size = ps5_texture_format_size(resource->base.format);
      staging_stride = (size_t)(unsigned)box->width * format_size;
      if (!format_size || level || box->width <= 0 || box->height <= 0 ||
          staging_stride > SIZE_MAX / (unsigned)box->height) {
         free(transfer);
         *out_transfer = NULL;
         return NULL;
      }
      staging_layer_stride = staging_stride * (unsigned)box->height;
      if (staging_layer_stride > SIZE_MAX / (unsigned)box->depth) {
         free(transfer);
         *out_transfer = NULL;
         return NULL;
      }
      staging_size = staging_layer_stride * (unsigned)box->depth;
      if (!ps5_transfer_alloc_staging(transfer, staging_size) ||
          (packed && (!resource->stencil_data ||
                      resource->stencil_allocation_size <
                         stencil_layer_size *
                         ps5_texture_level_layers(&resource->base, 0)))) {
         ps5_transfer_free_staging(transfer);
         free(transfer);
         *out_transfer = NULL;
         return NULL;
      }
      if (usage & PIPE_MAP_READ) {
         ps5_flush_gpu_data(resource->data, resource->allocation_size);
         if (packed)
            ps5_flush_gpu_data(resource->stencil_data,
                               resource->stencil_allocation_size);
         for (unsigned z = 0; z < (unsigned)box->depth; ++z) {
            const size_t depth_layer =
               ((size_t)(unsigned)box->z + z) * depth_layer_size;
            const size_t stencil_layer =
               ((size_t)(unsigned)box->z + z) * stencil_layer_size;

            for (unsigned y = 0; y < (unsigned)box->height; ++y) {
               for (unsigned x = 0; x < (unsigned)box->width; ++x) {
                  const size_t depth_offset = depth_layer +
                     ps5_tiled_depth_offset((unsigned)box->x + x,
                                            (unsigned)box->y + y,
                                            resource->base.width0, (unsigned)box->z + z);
                  uint8_t *pixel = (uint8_t *)transfer->staging +
                     z * staging_layer_stride + y * staging_stride +
                     x * format_size;

                  if (depth_offset > resource->allocation_size ||
                      resource->allocation_size - depth_offset < 4) {
                     ps5_transfer_free_staging(transfer);
                     free(transfer);
                     *out_transfer = NULL;
                     return NULL;
                  }
                  memset(pixel, 0, format_size);
                  memcpy(pixel, resource->data + depth_offset, 4);
                  if (packed) {
                     const size_t stencil_offset = stencil_layer +
                        ps5_tiled_stencil_offset((unsigned)box->x + x,
                           (unsigned)box->y + y, resource->base.width0,
                           (unsigned)box->z + z);

                     if (stencil_offset >=
                           resource->stencil_allocation_size) {
                        ps5_transfer_free_staging(transfer);
                        free(transfer);
                        *out_transfer = NULL;
                        return NULL;
                     }
                     pixel[4] = resource->stencil_data[stencil_offset];
                  }
               }
            }
         }
      }
      transfer->base.stride = staging_stride;
      transfer->base.layer_stride = staging_layer_stride;
      transfer->base.offset = 0;
      return transfer->staging;
   }

   if ((usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) &&
       (resource->base.bind & PIPE_BIND_RENDER_TARGET) &&
       !ps5_linear_sampled_layout(&resource->base)) {
      size_t staging_stride;
      size_t staging_size;
      unsigned y;

      format_size = ps5_texture_format_size(resource->base.format);
      if (level || box->depth != 1 ||
          !ps5_render_target_format(resource->base.format) ||
          (!(PS5_ENABLE_PADDED_FBO_CANDIDATE ||
             PS5_ENABLE_DYNAMIC_COLOR_TARGET_CANDIDATE) &&
           (resource->base.width0 != PS5_RENDER_WIDTH ||
            resource->base.height0 != PS5_RENDER_HEIGHT)) ||
          box->width <= 0 || box->height <= 0) {
         free(transfer);
         *out_transfer = NULL;
         return NULL;
      }
      staging_stride = (size_t)(unsigned)box->width * format_size;
      if (staging_stride > SIZE_MAX / (unsigned)box->height) {
         free(transfer);
         *out_transfer = NULL;
         return NULL;
      }
      staging_size = staging_stride * (unsigned)box->height;
      if (!ps5_transfer_alloc_staging(transfer, staging_size)) {
         free(transfer);
         *out_transfer = NULL;
         return NULL;
      }
      if (usage & PIPE_MAP_READ) {
         const size_t layer_base =
            (size_t)(unsigned)box->z * resource->layer_stride;

         ps5_flush_gpu_data(resource->data, resource->allocation_size);
         for (y = 0; y < (unsigned)box->height; ++y) {
            unsigned x;

            for (x = 0; x < (unsigned)box->width; ++x) {
               size_t tiled = layer_base + ps5_tiled_color_offset(
                  resource->base.format,
                  (unsigned)box->x + x, (unsigned)box->y + y,
                  ps5_tiled_rgba8_width(resource), (unsigned)box->z);

               if (tiled > resource->allocation_size ||
                   resource->allocation_size - tiled < format_size) {
                  ps5_transfer_free_staging(transfer);
                  free(transfer);
                  *out_transfer = NULL;
                  return NULL;
               }
               memcpy((uint8_t *)transfer->staging +
                         y * staging_stride + x * format_size,
                      resource->data + tiled, format_size);
            }
         }
      }
      transfer->base.stride = staging_stride;
      transfer->base.layer_stride = staging_size;
      transfer->base.offset = 0;
      return transfer->staging;
   }
   if ((usage & PIPE_MAP_READ) && resource->base.target == PIPE_BUFFER)
      ps5_flush_gpu_data(resource->data + offset, (size_t)box->width);
   return resource->data + offset;
}

static void
ps5_publish_transfer_write(struct pipe_transfer *transfer, const struct pipe_box *box)
{
   if (!transfer || transfer->resource->target != PIPE_BUFFER ||
       !(transfer->usage & PIPE_MAP_WRITE))
      return;
   struct ps5_resource *resource = (struct ps5_resource *)transfer->resource;
   const int x = box ? box->x : 0;
   const int width = box ? box->width : transfer->box.width;
   if (transfer->box.x < 0 || x < 0 || width <= 0 ||
       x > transfer->box.width || width > transfer->box.width - x)
      return;
   size_t offset = (size_t)transfer->box.x + (unsigned)x;
   if (offset <= resource->size && (size_t)width <= resource->size - offset)
      ps5_flush_gpu_data(resource->data + offset, (size_t)width);
}

static void
ps5_transfer_unmap(struct pipe_context *context,
                   struct pipe_transfer *transfer)
{
#if defined(PS5_NATIVE_TITLE_RUNTIME) && defined(PS5_DRAW_PROFILE)
   struct ps5_prepare_scope scope __attribute__((cleanup(ps5_prepare_scope_end))) =
      {7, ps5_prepare_clock()};
#endif

   struct ps5_transfer *ps5 = (struct ps5_transfer *)transfer;
   struct ps5_resource *resource =
      (struct ps5_resource *)transfer->resource;

   (void)context;
   if (resource->base.target != PIPE_BUFFER || (transfer->usage & PIPE_MAP_READ) ||
       !(transfer->usage & PIPE_MAP_UNSYNCHRONIZED))
      ps5_draw_batch_drain_buffer(transfer->resource);
   if (ps5->staging && (transfer->usage & PIPE_MAP_WRITE) &&
       (transfer->resource->bind & PIPE_BIND_DEPTH_STENCIL) &&
       !resource->depth_staging_size &&
       (transfer->resource->format == PIPE_FORMAT_Z32_FLOAT ||
        transfer->resource->format ==
           PIPE_FORMAT_Z32_FLOAT_S8X24_UINT)) {
      const bool packed = resource->base.format ==
                          PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
      const size_t depth_layer_size = ps5_tiled_depth_surface_size(
         resource->base.width0, resource->base.height0, 1);
      const size_t stencil_layer_size = ps5_tiled_stencil_surface_size(
         resource->base.width0, resource->base.height0);
      const unsigned format_size =
         ps5_texture_format_size(resource->base.format);

      for (unsigned z = 0; z < (unsigned)transfer->box.depth; ++z) {
         const size_t depth_layer =
            ((size_t)(unsigned)transfer->box.z + z) * depth_layer_size;
         const size_t stencil_layer =
            ((size_t)(unsigned)transfer->box.z + z) * stencil_layer_size;

         for (unsigned y = 0; y < (unsigned)transfer->box.height; ++y) {
            for (unsigned x = 0; x < (unsigned)transfer->box.width; ++x) {
               const size_t depth_offset = depth_layer +
                  ps5_tiled_depth_offset((unsigned)transfer->box.x + x,
                     (unsigned)transfer->box.y + y,
                     resource->base.width0, (unsigned)transfer->box.z + z);
               const uint8_t *pixel = (const uint8_t *)ps5->staging +
                  z * transfer->layer_stride + y * transfer->stride +
                  x * format_size;

               if (depth_offset <= resource->allocation_size &&
                   resource->allocation_size - depth_offset >= 4)
                  memcpy(resource->data + depth_offset, pixel, 4);
               if (packed) {
                  const size_t stencil_offset = stencil_layer +
                     ps5_tiled_stencil_offset(
                        (unsigned)transfer->box.x + x,
                        (unsigned)transfer->box.y + y,
                        resource->base.width0, (unsigned)transfer->box.z + z);

                  if (stencil_offset < resource->stencil_allocation_size)
                     resource->stencil_data[stencil_offset] = pixel[4];
               }
            }
         }
      }
      ps5_flush_gpu_data(resource->data, resource->allocation_size);
      if (packed)
         ps5_flush_gpu_data(resource->stencil_data,
                            resource->stencil_allocation_size);
   } else if (ps5->staging && (transfer->usage & PIPE_MAP_WRITE)) {
      size_t layer_base =
         (size_t)(unsigned)transfer->box.z * resource->layer_stride;
      unsigned format_size = ps5_texture_format_size(resource->base.format);

      for (unsigned y = 0; y < (unsigned)transfer->box.height; ++y) {
         for (unsigned x = 0; x < (unsigned)transfer->box.width; ++x) {
            const unsigned samples = MAX2(resource->base.nr_samples, 1u);
            for (unsigned sample = 0; sample < samples; ++sample) {
               size_t tiled = layer_base + (samples == 4
                  ? ps5_tiled_color_msaa4_offset(resource->base.format,
                       (unsigned)transfer->box.x + x, (unsigned)transfer->box.y + y,
                       sample, resource->base.width0, (unsigned)transfer->box.z)
                  : ps5_tiled_color_offset(resource->base.format,
                       (unsigned)transfer->box.x + x, (unsigned)transfer->box.y + y,
                       ps5_tiled_rgba8_width(resource), (unsigned)transfer->box.z));

               if (tiled <= resource->allocation_size &&
                   resource->allocation_size - tiled >= format_size)
                  memcpy(resource->data + tiled,
                         (uint8_t *)ps5->staging +
                            y * transfer->stride + x * format_size,
                         format_size);
            }
         }
      }
      ps5_flush_gpu_data(resource->data, resource->allocation_size);
   }
   if (!(transfer->usage & PIPE_MAP_FLUSH_EXPLICIT))
      ps5_publish_transfer_write(transfer, NULL);
   ps5_transfer_free_staging(ps5);
   free(ps5);
}
