#!/usr/bin/env python3
"""Execute pinned PS5 OpenGL transfer code with observable cache publication.

Usage: python3 tests/test_opengl_cache.py [--driver PS5_SCREEN_C] [--unpatched]

No GPU/hardware emulation. Actual unmap, publication and staging-free functions
are extracted unchanged. Gallium type declarations are reduced to fields used;
queue/cache operations are observed. Unused depth layout dependencies abort if
reached; the positive staged-color control is a 1x1 box at the origin, for which
all real tiled layouts address offset zero. Initialization and raw-map tails
are extracted snippets because compiling the full resource allocator requires
the native runtime. The private driver patch is applied in a temporary directory; source files are never edited.
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

def run_check():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', action='store_true', help=argparse.SUPPRESS)
    p.add_argument('--driver', type=Path, help='optional full pinned SDK ps5_screen.c')
    p.add_argument('--unpatched', action='store_true', help='run pinned negative control; expected failure')
    a = p.parse_args()
    source = (a.driver or root / 'tests/fixtures/ps5_opengl_cache.c').read_text()
    patch = root / 'tools/patches/ps5-opengl-cache-coherency.diff'
    if not a.unpatched:
        assert patch.is_file(), "Required PS5 OpenGL cache coherency patch is missing"
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / 'src/gallium/ps5/ps5_screen.c'
            target.parent.mkdir(parents=True)
            target.write_text(source)
            subprocess.run(['patch', '--batch', '-p1', '-i', str(patch)], cwd=folder, check=True, capture_output=True)
            source = target.read_text()

    def function(name):
        m = re.search(r'static [^\n]+\n' + name + r'\(', source)
        assert m, name
        return source[m.start():source.index('\n}\n', m.end()) + 3]

    structs = source[source.index('struct ps5_resource {'):source.index('struct ps5_vertex_elements {')] if 'struct ps5_vertex_elements {' in source else source[source.index('struct ps5_resource {'):source.index('      memset(resource->data, 0, size);')]
    map_body = function('ps5_transfer_map')
    start = map_body.index('   if ((usage & PIPE_MAP_READ) && resource->base.target == PIPE_BUFFER)')
    raw_tail = map_body[start:]
    before_access = map_body[map_body.index("   /* Mesa's streaming uploader"):map_body.index('   if (usage & PIPE_MAP_PERSISTENT)')]
    start = source.index('   memset(resource->data, 0, allocation_size);')
    end = source.index('   if (templ->format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT)', start)
    publish_start = source.index('   resource->depth_staging_size = depth_staging_size;')
    publish_end = source.index('   if (PS5_ENABLE_SHARED_RENDER_POOL_CANDIDATE', publish_start)
    publication = source[publish_start + len('   resource->depth_staging_size = depth_staging_size;\n'):publish_end]
    finish_initialization = (';\n   resource->allocation_size = allocation_size;\n' + publication)
    allocation = '    if (false) goto primary_ready;\n' + source[start:end] + finish_initialization
    label = source[source.index('primary_ready:', start):end]
    start = source.index('      memset(resource->data, 0, size);')
    arena = source[start:source.index('#ifdef AGC_RUNTIME_DIAGNOSTICS', start)] + '\n    goto primary_ready;\n' + label + finish_initialization

    code = r'''
    #include <assert.h>
    #include <stdbool.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <sys/mman.h>
    #define PIPE_MAX_TEXTURE_LEVELS 16
    #define MAX2(a,b) ((a) > (b) ? (a) : (b))
    enum { PIPE_BUFFER, PIPE_TEXTURE_2D };
    enum { COLOR, PIPE_FORMAT_Z32_FLOAT, PIPE_FORMAT_Z32_FLOAT_S8X24_UINT };
    enum { PIPE_MAP_READ=1, PIPE_MAP_WRITE=2, PIPE_MAP_UNSYNCHRONIZED=4,
           PIPE_MAP_FLUSH_EXPLICIT=8, PIPE_BIND_DEPTH_STENCIL=16 };
    struct pipe_resource { unsigned target, bind, format, width0, height0, nr_samples; };
    struct pipe_context { unsigned unused; };
    struct pipe_box { int x,y,z,width,height,depth; };
    struct pipe_transfer { struct pipe_resource *resource; unsigned usage,level,stride,layer_stride;
        struct pipe_box box; };
    ''' + structs + r'''
    /* GPU memory bypasses the CPU cache. A flush writes dirty 64-byte CPU lines
     * back, then invalidates them. This reproduces picture corruption, not just
     * an absent function call. */
    static uint8_t gpu_memory[256];
    static uint8_t *cpu_memory;
    static bool dirty[4], valid[4];
    static const void *flushed;
    static size_t flushed_size;
    static unsigned flushes, drains;
    static void ps5_flush_gpu_data(const void *p, size_t n) {
        if (!n) return; /* actual driver treats zero-length publication as a no-op */
        assert(p && (const uint8_t *)p>=cpu_memory && (const uint8_t *)p+n<=cpu_memory+256);
        flushed=p; flushed_size=n; ++flushes;
        size_t first=((const uint8_t *)p-cpu_memory)/64;
        size_t last=((const uint8_t *)p-cpu_memory+n-1)/64;
        for(size_t i=first;i<=last;++i) {
            if(dirty[i])memcpy(gpu_memory+i*64,cpu_memory+i*64,64);
            dirty[i]=valid[i]=false;
        }
    }
    static void cpu_store(size_t offset,uint32_t value) {
        size_t line=offset/64;
        if(!valid[line]) {memcpy(cpu_memory+line*64,gpu_memory+line*64,64);valid[line]=true;}
        memcpy(cpu_memory+offset,&value,4);dirty[line]=true;
    }
    static uint32_t gpu_word(size_t offset) {uint32_t v;memcpy(&v,gpu_memory+offset,4);return v;}
    static void *cache_memcpy(void *to,const void *from,size_t n) {
        const uintptr_t address=(uintptr_t)to,begin=(uintptr_t)cpu_memory;
        if(address>=begin && address<begin+256 && n) {
            assert(n<=begin+256-address);
            const size_t first=(address-begin)/64,last=(address-begin+n-1)/64;
            for(size_t i=first;i<=last;++i) {
                if(!valid[i]) {memcpy(cpu_memory+i*64,gpu_memory+i*64,64);valid[i]=true;}
                dirty[i]=true;
            }
        }
        return memcpy(to,from,n);
    }
    #define memcpy cache_memcpy
    static void ps5_draw_batch_drain_buffer(struct pipe_resource *p) { assert(p); ++drains; }
    static void ps5_invalidate_gpu_writes(struct ps5_resource *r) {
        if(r && r->gpu_written) {r->gpu_written=false;ps5_flush_gpu_data(r->data,r->allocation_size);}
    }
    static size_t ps5_tiled_depth_surface_size(unsigned w,unsigned h,unsigned s) { (void)w;(void)h;(void)s;abort(); }
    static size_t ps5_tiled_stencil_surface_size(unsigned w,unsigned h) { (void)w;(void)h;abort(); }
    static size_t ps5_tiled_depth_offset(unsigned x,unsigned y,unsigned w,unsigned z) { (void)x;(void)y;(void)w;(void)z;abort(); }
    static size_t ps5_tiled_stencil_offset(unsigned x,unsigned y,unsigned w,unsigned z) { (void)x;(void)y;(void)w;(void)z;abort(); }
    static unsigned ps5_texture_format_size(unsigned f) { assert(f==COLOR); return 4; }
    static unsigned ps5_tiled_rgba8_width(const struct ps5_resource *r) { return r->base.width0; }
    static size_t ps5_tiled_color_offset(unsigned f,unsigned x,unsigned y,unsigned w,unsigned z) {
        assert(f==COLOR && !x && !y && w==1 && !z); return 0;
    }
    static size_t ps5_tiled_color_msaa4_offset(unsigned f,unsigned x,unsigned y,unsigned s,unsigned w,unsigned z) {
        (void)f;(void)x;(void)y;(void)s;(void)w;(void)z;abort();
    }
    ''' + function('ps5_transfer_free_staging') + function('ps5_publish_transfer_write') + function('ps5_transfer_unmap') + r'''
    static void before_texture_access(struct ps5_resource *resource,unsigned usage) {
        struct pipe_resource *base=&resource->base;
    ''' + before_access + r'''
    }
    static void *raw_map_tail(struct ps5_resource *resource, unsigned usage, size_t offset, const struct pipe_box *box) {
    ''' + raw_tail + r'''
    static void initialize(struct ps5_resource *resource,size_t allocation_size) {
    ''' + allocation + r'''
    }
    static void initialize_arena(struct ps5_resource *resource,size_t size) {
        const size_t allocation_size = size;
        (void)allocation_size;
    ''' + arena + r'''
    }
    static int fail(const char *s) { fprintf(stderr,"FAIL: %s\n",s); return 1; }
    int main(int argc,char **argv) {
        assert(argc==2);
        uint32_t data[64]={0};cpu_memory=(uint8_t *)data;
        memset(gpu_memory,0xa5,sizeof(gpu_memory));
        for(unsigned i=0;i<4;++i)dirty[i]=valid[i]=true;
        struct ps5_resource r={.base={.target=PIPE_TEXTURE_2D,.format=COLOR,.width0=1},
            .data=(uint8_t *)data,.size=128,.allocation_size=128};
        const char *which=argv[1];
        if (!strcmp(which,"allocation") || !strcmp(which,"arena") || !strcmp(which,"stencil-init")) {
            /* Dedicated creation still has calloc-zero allocation_size at
             * primary_ready. The arena helper has already assigned its field. */
            r.allocation_size = !strcmp(which,"arena") ? 128 : 0;
            if(!strcmp(which,"stencil-init")) {r.stencil_data=cpu_memory+128;r.stencil_allocation_size=128;}
            if (!strcmp(which,"arena")) initialize_arena(&r,128);
            else initialize(&r,128);
            /* A new image is copied by the GPU. A later sample/read invalidation
             * must not write initialization zeros over either cacheline. */
            memset(gpu_memory,0x5a,sizeof(gpu_memory));
            ps5_flush_gpu_data(data,sizeof(data));
            const size_t begin=!strcmp(which,"stencil-init") ? 128 : 0;
            for(size_t i=begin;i<begin+128;++i)
                if(gpu_memory[i]!=0x5a)return fail("dirty initialization zeros overwrite GPU picture");
        } else if (!strcmp(which,"raw-map")) {
            /* A clean CPU cacheline predates a GPU write. Edit one texel while
             * preserving adjacent GPU-written texels in the same line. */
            dirty[0]=dirty[1]=false;
            memset(data,0x11,sizeof(data));memset(gpu_memory,0x22,sizeof(gpu_memory));
            struct pipe_box b={.width=1,.height=1,.depth=1};
            before_texture_access(&r,PIPE_MAP_WRITE);
            assert(raw_map_tail(&r,PIPE_MAP_WRITE,4,&b)==r.data+4);
            cpu_store(4,0x12345678);
            struct ps5_transfer *t=calloc(1,sizeof(*t));assert(t);
            t->base.resource=&r.base;t->base.usage=PIPE_MAP_WRITE;t->base.box=b;
            ps5_transfer_unmap(NULL,&t->base);
            if(gpu_word(4)!=0x12345678)return fail("raw texture CPU update is invisible to GPU after unmap");
            if(gpu_word(0)!=0x22222222 || gpu_word(8)!=0x22222222 || gpu_word(64)!=0x22222222)
                return fail("partial CPU update overwrites neighbouring GPU texels with stale cache bytes");
        } else {
            struct ps5_transfer *t=calloc(1,sizeof(*t)); assert(t);
            t->base.resource=&r.base; t->base.usage=PIPE_MAP_WRITE;
            t->base.box=(struct pipe_box){.width=1,.height=1,.depth=1};
            unsigned expected=1; size_t expected_size=128;
            if (!strcmp(which,"buffer")) {
                r.base.target=PIPE_BUFFER; t->base.box.x=4;t->base.box.width=12;
                expected_size=12;
            } else if (!strcmp(which,"staged") || !strcmp(which,"staged-neighbours")) {
                if(!strcmp(which,"staged-neighbours")) {
                    dirty[0]=dirty[1]=false;
                    memset(data,0x11,sizeof(data));memset(gpu_memory,0x22,sizeof(gpu_memory));
                    before_texture_access(&r,PIPE_MAP_WRITE);
                    expected=2;
                }
                t->staging=malloc(4); assert(t->staging); *(uint32_t *)t->staging=0x12345678;
                t->base.stride=4;
            } else if (!strcmp(which,"readonly")) {
                t->base.usage=PIPE_MAP_READ; expected=0;
            } else if (!strcmp(which,"explicit")) {
                t->base.usage|=PIPE_MAP_FLUSH_EXPLICIT; expected=0;
            } else assert(!strcmp(which,"linear"));
            ps5_transfer_unmap(NULL,&t->base);
            if(!strcmp(which,"staged-neighbours") &&
               (gpu_word(0)!=0x12345678 || gpu_word(4)!=0x22222222 || gpu_word(8)!=0x22222222))
                return fail("staged partial write overwrites GPU neighbour texels with stale cache bytes");
            if (flushes!=expected) return fail("texture CPU write unmap does not publish its cache lines");
            if (expected && (flushed!=(r.base.target==PIPE_BUFFER ? r.data+4:r.data) || flushed_size!=expected_size))
                return fail("publication range is incorrect");
            if (!strcmp(which,"staged") && gpu_word(0)!=0x12345678) return fail("staged data not published");
        }
        printf("PASS: %s\n",which);return 0;
    }
    '''

    with tempfile.TemporaryDirectory() as tmp:
        file = Path(tmp) / 'publication.c'
        exe = Path(tmp) / 'publication'
        file.write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(file), '-o', str(exe)], check=True)
        failures = []
        for case in ('buffer', 'staged', 'staged-neighbours', 'readonly', 'explicit', 'linear', 'raw-map', 'allocation', 'arena', 'stencil-init'):
            result = subprocess.run([str(exe), case], text=True, capture_output=True)
            print(result.stdout + result.stderr, end='')
            if result.returncode:
                failures.append(case)
        print(('pinned' if a.unpatched else 'patched') + ': ' + (', '.join(failures) + ' FAIL' if failures else 'PASS'))
        raise SystemExit(bool(failures))


import sys
import unittest


class TestOpenGlCache(unittest.TestCase):
    def run_case(self, *args):
        return subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run', *args],
                              text=True, capture_output=True)

    def test_patched_texture_ownership(self):
        result = self.run_case()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('patched: PASS', result.stdout)

    def test_pinned_negative_control_exposes_corruption(self):
        result = self.run_case('--unpatched')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for failure in ('texture CPU write unmap does not publish',
                        'raw texture CPU update is invisible',
                        'dirty initialization zeros overwrite GPU picture'):
            self.assertIn(failure, result.stdout + result.stderr)
        self.assertIn('PASS: staged', result.stdout)
        self.assertIn('PASS: buffer', result.stdout)

    def test_publication_without_map_invalidation_loses_neighbour_texels(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / 'src/gallium/ps5/ps5_screen.c'
            target.parent.mkdir(parents=True)
            target.write_text((root / 'tests/fixtures/ps5_opengl_cache.c').read_text())
            subprocess.run(['patch', '--batch', '-p1', '-i',
                            str(root / 'tools/patches/ps5-opengl-cache-coherency.diff')],
                           cwd=folder, check=True, capture_output=True)
            source = target.read_text()
            before = ('   if (base->target != PIPE_BUFFER && (usage & (PIPE_MAP_READ | PIPE_MAP_WRITE))) {\n'
                      '      ps5_flush_gpu_data(resource->data, resource->allocation_size);\n'
                      '      if (resource->stencil_data)\n'
                      '         ps5_flush_gpu_data(resource->stencil_data, resource->stencil_allocation_size);\n'
                      '   }')
            self.assertEqual(source.count(before), 1)
            target.write_text(source.replace(before, ''))
            result = self.run_case('--driver', str(target), '--unpatched')
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn('partial CPU update overwrites neighbouring GPU texels',
                          result.stdout + result.stderr)
            self.assertIn('PASS: linear', result.stdout)


if __name__ == '__main__':
    if '--run' in sys.argv or '--driver' in sys.argv or '--unpatched' in sys.argv:
        run_check()
    else:
        unittest.main()
