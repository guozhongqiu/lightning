#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"
#include "mem/slab.h"

//#define SLAB_SEG (1024 * 1024 * 2)
#define SLAB_SEG  (ltgconf_global.nr_hugepage ? MAX_ALLOC_SIZE : (1024 * 1024 * 2))
#define SLAB_MD sizeof(slab_md_t)

#if ENABLE_HUGEPAGE
static void *__slab_lowlevel_calloc(int _private)
{
        int ret;
        void *ptr;
        uint32_t size = SLAB_SEG;

        (void) _private;

        ret = hugepage_getfree(&ptr, &size, __FUNCTION__);
        if (ret)
                return NULL;
        else
                return ptr;
}

#if 0
static void __slab_lowlevel_free(void *ptr, int _private)
{
        (void) ptr;
        (void) _private;

        UNIMPLEMENTED(__DUMP__);
}
#endif

#else

static void *__slab_lowlevel_calloc(int _private)
{
        int ret;
        void *ptr;

        (void) _private;
        
        ret = ltg_malloc(&ptr, SLAB_SEG);
        if (ret)
                return NULL;
        else
                return ptr;
}

#if 0
static void __slab_lowlevel_free(void *ptr, int _private)
{
        (void) _private;

        ltg_free(&ptr);
}
#endif

#endif

static int __slab_init__(const char *name, slab_bucket_t *slab, int split,
                         pid_t tid, int _private)
{
        memset(slab, 0x0, sizeof(*slab));

        slab->split = split;
        slab->tid = tid;
        slab->_private = _private;
        INIT_LIST_HEAD(&slab->list);
        INIT_LIST_HEAD(&slab->used);
        strcpy(slab->name, name);

        DBUG("init split %u %s\n", slab->split, name);

        return 0;
}

static int __slab_init(const char *name, slab_array_t **_array, int _private,
                       int min, int shift, uint32_t magic)
{
        int ret;
        slab_array_t *array;
        size_t size = sizeof(*array) + sizeof(slab_bucket_t) * shift;

        LTG_ASSERT(size <= SLAB_SEG);

        array = __slab_lowlevel_calloc(_private);
        if (array == NULL) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        array->count = shift;
        array->magic = magic;
        if (_private) {
                core_t *core = core_self();
                array->coreid = core->hash;
                array->tid = __gettid();
        } else {
                array->coreid = -1;
                array->tid = -1;
        }

        ret = ltg_spin_init(&array->spin);
        if (ret)
                GOTO(err_ret, ret);

        for (int i = 0; i < shift; i++) {
                slab_bucket_t *mem = &array->slab_bucket[i];
                ret = __slab_init__(name, mem, min * (1 << i),
                                    array->tid, _private);
                if (ret)
                        GOTO(err_ret, ret);
        }

        *_array = array;

        return 0;
err_ret:
        return ret;
}

int slab_private_init(const char *name, slab_t **_slab, slab_array_t *_public, int min,
                      int shift, uint32_t magic)
{
        int ret;
        slab_t *slab;

        ret = ltg_malloc((void **)&slab, sizeof(*slab));
        if (ret)
                GOTO(err_ret, ret);

        memset(slab, 0x0, sizeof(*slab));
        
        ret = __slab_init(name, &slab->_private, 1, min, shift, magic);
        if (ret)
                GOTO(err_ret, ret);

        slab->_public = _public;
        slab->max = min * (1 << shift);
        *_slab = slab;
        
        return 0;
err_ret:
        return ret;
}

int slab_init(const char *name, slab_t **_slab, slab_array_t **_public, int min,
              int shift, uint32_t magic)
{
        int ret;
        slab_t *slab;

        ret = ltg_malloc((void **)&slab, sizeof(*slab));
        if (ret)
                GOTO(err_ret, ret);

        memset(slab, 0x0, sizeof(*slab));
        
        ret = __slab_init(name, &slab->_public, 0, min, shift, magic);
        if (ret)
                GOTO(err_ret, ret);

        slab->max = min * (1 << shift);
        *_public = slab->_public;
        *_slab = slab;
        
        return 0;
err_ret:
        return ret;
}

static int __slab_extend(slab_bucket_t *slab, uint32_t magic, uint32_t coreid)
{
        int ret;
        void *ptr;
        core_t *core = core_self();

        if (slab->count == SLAB_SEG_MAX) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }
        
        ptr = __slab_lowlevel_calloc(core ? 1 : 0);
        if (ptr == NULL) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        slab->array[slab->count] = ptr;
        slab->count++;

        slab_md_t *md;
        int count = SLAB_SEG / (slab->split + SLAB_MD);
        for (int i = 0; i < count; i++ ) {
                md = ptr + i * (slab->split + SLAB_MD);
                md->magic = magic;
                md->slab_bucket = slab;
                md->coreid = coreid;
                md->seghead = ptr;
                list_add_tail(&md->hook, &slab->list);
        }

        if (likely(core)) {
                DINFO("%s[%d] %s split %u count %u, total %u\n",
                      core->name, core->hash, slab->name, slab->split, count,
                      slab->count);
        } else {
                DINFO("none[-1] %s split %u count %u, total %u\n",
                      slab->name, slab->split, count,
                      slab->count);
        }

        return 0;
err_ret:
        return ret;
}

static void S_LTG *__slab_alloc__(slab_bucket_t *slab, uint32_t magic, uint32_t coreid)
{
        int ret;
        slab_md_t *md;

        if (unlikely(list_empty(&slab->list))) {
                ret = __slab_extend(slab, magic, coreid);
                if (ret)
                        return NULL;
        }

        DBUG("alloc %ju\n", slab->split);

        md = (void *)slab->list.next;
        md->time = gettime();
        list_del(&md->hook);
        list_add_tail(&md->hook, &slab->used);

        return md->ptr;
}

inline static void S_LTG  *__slab_alloc(slab_array_t *array, size_t size)
{
        for (int i = 0; i < array->count; i++) {
                slab_bucket_t *slab = &array->slab_bucket[i];
                if (slab->split >= size) {
                        return __slab_alloc__(slab, array->magic, array->coreid);
                }
        }

        return NULL;
}

void *slab_alloc_glob(slab_t *slab, size_t size)
{
        int ret;
        void *ptr;

        LTG_ASSERT(size <= slab->max);

        ret = ltg_spin_lock(&slab->_public->spin);
        if (ret)
                GOTO(err_ret, ret);

        ptr = __slab_alloc(slab->_public, size);

        ltg_spin_unlock(&slab->_public->spin);

        return ptr;
err_ret:
        return NULL;
}

void S_LTG *slab_alloc(slab_t *slab, size_t size)
{
        int ret;
        void *ptr;

        LTG_ASSERT(size <= slab->max);

        if (likely(slab->_private)) {
                ptr =  __slab_alloc(slab->_private, size);
        } else {
                ret = ltg_spin_lock(&slab->_public->spin);
                if (ret)
                        GOTO(err_ret, ret);

                ptr = __slab_alloc(slab->_public, size);

                ltg_spin_unlock(&slab->_public->spin);
        }

        return ptr;
err_ret:
        return NULL;
}

void S_LTG __slab_free_local(void *ptr)
{
        slab_md_t *md = ptr - SLAB_MD;
        slab_bucket_t *slab_bucket = md->slab_bucket;

        LTG_ASSERT(slab_bucket->_private);

        list_del(&md->hook);
        list_add_tail(&md->hook, &slab_bucket->list);
}

void S_LTG __slab_free_public(slab_t *slab, void *ptr)
{
        int ret;
        slab_md_t *md = ptr - SLAB_MD;
        slab_bucket_t *slab_bucket = md->slab_bucket;

        LTG_ASSERT(slab_bucket->_private == 0);

        ret = ltg_spin_lock(&slab->_public->spin);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        list_del(&md->hook);
        list_add_tail(&md->hook, &slab_bucket->list);

        ltg_spin_unlock(&slab->_public->spin);
}

static int __slab_cross_free_va(va_list ap)
{
        void *ptr = va_arg(ap, void *);
        slab_md_t *md = ptr - SLAB_MD;

        LTG_ASSERT(md->slab_bucket->_private);

        va_end(ap);

        __slab_free_local(ptr);
 
        return 0;
}

void S_LTG slab_free(slab_t *slab, void *ptr)
{
        slab_md_t *md = ptr - SLAB_MD;

        if (likely(slab->_private && md->coreid == slab->_private->coreid)) {
                slab_array_t *array = slab->_private;
                LTG_ASSERT(md->magic == array->magic);
                LTG_ASSERT(md->slab_bucket->tid == array->tid);

                __slab_free_local(ptr);
        } else if (!md->slab_bucket->_private) {
                slab_array_t *array = slab->_public;
                LTG_ASSERT(md->magic == array->magic);
                LTG_ASSERT(md->slab_bucket->tid == array->tid);
                
                __slab_free_public(slab, ptr);
        } else {
                DBUG("cross free %p\n", ptr);

                //slab_array_t *array = slab->_private;
                //LTG_ASSERT(md->magic == array->magic);
                //LTG_ASSERT(md->slab_bucket->tid == array->tid);

                core_request(md->coreid, -1, "slab_free",
                             __slab_cross_free_va, ptr);
                
        }
}

void slab_scan(void *_core, slab_array_t *array)
{
        struct list_head *pos;
        time_t now = gettime();
        slab_md_t *md;
        core_t *core = _core;

        uint32_t seq = 0;
        for (int i = 0; i < array->count; i++) {
                slab_bucket_t *slab_bucket = &array->slab_bucket[i];
                
                list_for_each(pos, &slab_bucket->used) {
                        md = (void *)pos;

                        if (now - md->time > 256) {
                                DWARN("%s[%d],addr %p used %u size %u, seq[%d]\n",
                                      core->name, core->hash, md->ptr,
                                      now - md->time, slab_bucket->split, seq);
                                seq++;
                        }
                }
        }
}
