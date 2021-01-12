#include <numaif.h>
#include <sys/mman.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "core/core.h"

#define IS_POWER_OF_2(x) (!((x)&((x)-1)))

typedef struct {
        ltg_spinlock_t    lock;

        void             *malloc_addr;
        void             *start_addr;
        void             *private_hp_head[CORE_MAX]; /*polling core hugepage head*/

        int              hash;
        int              hugepage_count;
        int              nr_alloc;
} hugepage_head_t;

#define HUGEPAGE_HEAD_DUMP_L(LEVEL, head, format, a...) do { \
    LEVEL("hugepage head %p addr %p %p hash %d count %d/%d  "format, \
          (head),  \
          (head)->malloc_addr,  \
          (head)->start_addr,  \
          (head)->hash,  \
          (head)->hugepage_count,  \
          (head)->nr_alloc,  \
          ##a \
          ); \
} while(0)

#define HUGEPAGE_HEAD_DUMP(head, format, a...) HUGEPAGE_HEAD_DUMP_L(DBUG, head, format, ##a)


static hugepage_head_t *__hugepage__ = NULL;
static __thread hugepage_head_t *__private_huge__ = NULL;
struct mem_alloc *hugepage_alloc_ops = NULL;
static int __use_huge__ = 0;
static int PRIVATE_HP_COUNT = 0;
static int PUBLIC_HP_COUNT = 0;

static void __hugepage_head_init(hugepage_head_t *head, void *mem, void *addr,
                                 int hp_count, int sockid)
{
        long unsigned int sock;

        if (sockid >= 0){
                sock = 1 << sockid;
                mbind(addr, HUGEPAGE_SIZE, MPOL_PREFERRED, &sock, 3, 0);
        }

        memset(addr, 0x00, HUGEPAGE_SIZE);

        head->start_addr = addr + HUGEPAGE_SIZE;
        head->hugepage_count = hp_count;
        head->nr_alloc = 0;

        head->malloc_addr = mem;
        head->hash = -1;
        
        int ret = ltg_spin_init(&head->lock);
        LTG_ASSERT(ret == 0);
}

static int __map_hugepages(void *addr, int hp_count)
{
        int i, j, ret;
        char *map_ret;
        void *map_addr = addr;
        void *unmap_addr = addr;

        DINFO("hp_count %d\n", hp_count);

        for (i = 0; i < hp_count; i++) {
                map_ret = mmap(map_addr, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_HUGETLB|MAP_PRIVATE |MAP_FIXED|MAP_ANONYMOUS|0x40000 , -1, 0);
                if (map_ret == MAP_FAILED) {
                        DINFO("mmap erro  %p %d %s\n", map_addr, hp_count, strerror(errno))
                                ret = ENOMEM;
                        GOTO(err_ret, ret);
                }

                map_addr += HUGEPAGE_SIZE;
        }

        return 0;

err_ret:
        for (j = 0; j < i; j++) {
                munmap(unmap_addr, HUGEPAGE_SIZE);
                unmap_addr += HUGEPAGE_SIZE;
        }
        return ret;
}

static void __hugepage_init(void *addr, int sockid)
{
        unsigned long int  sock;

        
        if (sockid >= 0){
                sock = 1 << sockid;
                mbind(addr, HUGEPAGE_SIZE, MPOL_PREFERRED, &sock, 3, 0);
        }

        memset(addr, 0x00, HUGEPAGE_SIZE);

        return ;
}

void *hugepage_private_init(int hash, int sockid)
{
        hugepage_head_t *head;
        int i ;

        if (__hugepage__ == NULL)
                return NULL;

        void *addr = __hugepage__->private_hp_head[hash];
        
        LTG_ASSERT(addr);

        DINFO("hash %d head addr %p\n", hash, addr);
        head = (hugepage_head_t *)addr;

        __hugepage_head_init(head, NULL, addr, PRIVATE_HP_COUNT, sockid);

        for (i = 0; i < PRIVATE_HP_COUNT; i++) {
                addr += HUGEPAGE_SIZE;
                __hugepage_init(addr, sockid);
        }

        head->hash = hash;

        __private_huge__ = head;
        hugepage_alloc_ops->init((void *)head + sizeof(*head), PRIVATE_HP_COUNT);

        HUGEPAGE_HEAD_DUMP_L(DINFO, head, "\n");
        return head;
}

static void __hugepage_init_public(hugepage_head_t *head, void *_public)
{
        void *pos = _public;

        DINFO("hp_count %d\n", PUBLIC_HP_COUNT);

        for (int i = 0; i < PUBLIC_HP_COUNT; i++) {
                __hugepage_init(pos,  -1);
                pos += HUGEPAGE_SIZE;
        }

        __hugepage__ = head;

        HUGEPAGE_HEAD_DUMP_L(DINFO, head, "\n");
}

static void __hugepage_init_private(hugepage_head_t *head, void *_private)
{
        void *pos = _private;
        
        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_used(i)) {
                        head->private_hp_head[i] = NULL;
                        continue;
                }
                
                head->private_hp_head[i] = pos;
                pos += (PRIVATE_HP_COUNT  + 1) * HUGEPAGE_SIZE;

                DINFO("core[%d] count %d head 0x%p\n", i, PRIVATE_HP_COUNT + 1, pos);
        }
}

int hugepage_init(int daemon, uint64_t coremask, int nr_hugepage)
{ 
        int ret, hp_count, poll_num = 0;
        size_t mem_size;
        void *mem, *addr, *_private, *_public;
        size_t align;
        hugepage_head_t *head;

        if (nr_hugepage == 0 || daemon == 0) {
                posix_memalloc_reg();
                hugepage_alloc_ops->init(NULL, 0);
                return 0;
        } else {
                buddy_memalloc_reg();
        }
        
        __use_huge__ = nr_hugepage;
        PRIVATE_HP_COUNT = nr_hugepage;
        PUBLIC_HP_COUNT = nr_hugepage / 2;

        if (!IS_POWER_OF_2(PRIVATE_HP_COUNT)) {
                DERROR("private hp count error %d\n", nr_hugepage);
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        if (!IS_POWER_OF_2(PUBLIC_HP_COUNT)) {
                DERROR("private hp count error %d\n", nr_hugepage);
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        static_assert(sizeof(*head) <= HUGEPAGE_SIZE, "hugepage");
        
        
        for (int i = 0; i < CORE_MAX; i++) {
                if (core_usedby(coremask, i))
                        poll_num++;
        }

        mem_size = ((LLU)PRIVATE_HP_COUNT + 1) * HUGEPAGE_SIZE * poll_num
                       + (PUBLIC_HP_COUNT + 2) * HUGEPAGE_SIZE;
        
        hp_count = (PRIVATE_HP_COUNT + 1) * poll_num
                + PUBLIC_HP_COUNT + 1;

        DINFO("private_hp_count %d poll_num %d hp_count %d\n",
              PRIVATE_HP_COUNT, poll_num, hp_count);

        mem = malloc(mem_size);
        if (mem == NULL) {
                DINFO("malloc %d error\n", mem_size);
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        align = (size_t)mem & (HUGEPAGE_SIZE - 1);
        if (align){
                addr = mem + HUGEPAGE_SIZE - align;
        } else
                addr = mem;
        
        ret = __map_hugepages(addr, hp_count);
        if (ret)
                GOTO(err_ret, ret);

        head = addr;
        _public = addr + HUGEPAGE_SIZE;
        _private = _public + (PUBLIC_HP_COUNT) * HUGEPAGE_SIZE;

        __hugepage_head_init(head, mem, addr, hp_count - 1, -1);

        __hugepage_init_public(head, _public);
        hugepage_alloc_ops->init((void *)head + sizeof(*head), PUBLIC_HP_COUNT);

        __hugepage_init_private(head, _private);

        return 0;
err_ret:
        return ret;
}

void private_hugepage_free(void *addr)
{   
        (void)addr;

        return ;
}

int hugepage_getfree(void **_addr, uint32_t *size, const char *caller)
{
        int ret;
        hugepage_head_t *head = __private_huge__ ? __private_huge__ : __hugepage__;

        if (*size % HUGEPAGE_SIZE)
                return EINVAL;

        if (head == NULL) {
                ret = hugepage_alloc_ops->alloc(NULL, _addr, size);
                if (ret)
                        GOTO(err_ret, ret);
        } else {
                if (unlikely(head == __hugepage__))
                        ltg_spin_lock(&head->lock);

                ret = hugepage_alloc_ops->alloc((void *)head + sizeof(*head), _addr, size);
                if (ret)
                        GOTO(err_ret, ret);

                head->nr_alloc += *size / HUGEPAGE_SIZE;

                HUGEPAGE_HEAD_DUMP_L(DINFO, head, "caller %s size %u\n", caller, *size);

                if (unlikely(head == __hugepage__))
                        ltg_spin_unlock(&head->lock);
        }

        return 0;
err_ret:
        return ret;
}

int hugepage_get()
{
        hugepage_head_t *head = __private_huge__ ? __private_huge__ : __hugepage__;
        if (head != NULL) {
                if (head == __hugepage__)
                        return PUBLIC_HP_COUNT;
                else
                        return head->hugepage_count;
        }

        return 0;
}

void hugepage_show(const char *caller)
{
        hugepage_head_t *head = __private_huge__ ? __private_huge__ : __hugepage__;
        if (head != NULL) {
                if (unlikely(head == __hugepage__))
                        ltg_spin_lock(&head->lock);

                HUGEPAGE_HEAD_DUMP_L(DINFO, head, "caller %s\n", caller);

                if (unlikely(head == __hugepage__))
                        ltg_spin_unlock(&head->lock);
        }
}

int suzaku_mem_alloc_register(struct mem_alloc *alloc_ops)
{
        DINFO("register ops %p %p\n", alloc_ops, hugepage_alloc_ops);
        if (hugepage_alloc_ops != NULL)
                LTG_ASSERT(0);

        hugepage_alloc_ops = alloc_ops;

        return 0;
}

void get_global_private_mem(void **private_mem, uint64_t *private_mem_size)
{
        if (__hugepage__ == NULL)
                return ;

        *private_mem = __hugepage__->start_addr;
        *private_mem_size = ((uint64_t)__hugepage__->hugepage_count) * HUGEPAGE_SIZE;
}
