#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>

#define MP_ALIGNMENT            32                      // 内存对齐
#define MP_PAGE_SIZE            4096
#define MP_MAX_ALLOC_FROM_POOL  (MP_PAGE_SIZE - 1)      // 视为大块和小块的分界线

// 根据内存对齐找到下一个写入数据的地址
#define mp_align(n, alignment) (((n) + (alignment - 1)) & ~(alignment - 1))
#define mp_align_ptr(p, alignment) (void *)((((size_t) p) + (alignment - 1)) & ~(alignment - 1))

// 内存池结构
// 大块内存分配
struct mp_large_s {
    struct mp_large_s *next; // 指向下一块大块内存
    void *alloc; // 指向分配的大块内存
};
/* 
小块内存分配
原理: 先分配一整个块, 例如 4K, 然后在这 4K 的大块中依次分配
*/
struct mp_node_s {
    unsigned char *last; // 当前内存分配的结束位置, 下一次分配从这里开始分配
    unsigned char *end; // 整个分配的大块内存最后的位置

    struct mp_node_s *next; // 指向下一个存小块的内存
    size_t failed; // 内存池分配失败的次数
};
/*
小块内存采用尾插法, 因此需要记录一个头结点进行删除、释放

大块内存之间互不影响，因此采用头插法, 新插入的块作为头, large 始终指向头结点的位置, 因此不需要记录头结点就可以删除、释放
*/
struct mp_pool_s {
    size_t max; // 内存池数据块的最大值, 超过 max 视为大块, 否则放入小块中

    struct mp_node_s *head; // 指向小块内存的头结点
    struct mp_node_s *current; // 指向当前小块内存池

    struct mp_large_s *large; // 大块内存链表, 即分配空间超过 max 的内存

};

struct mp_pool_s *mp_create_pool(size_t size);
void mp_destory_pool(struct mp_pool_s *pool);
void *mp_alloc(struct mp_pool_s *pool, size_t size);
void *mp_nalloc(struct mp_pool_s *pool, size_t size);
void *mp_calloc(struct mp_pool_s *pool, size_t size);
void mp_free(struct mp_pool_s *pool, void *p);

// 创建内存池
struct mp_pool_s *mp_create_pool(size_t size) { 

    struct mp_pool_s *p;
    int ret = posix_memalign((void **)&p, MP_ALIGNMENT, size + sizeof(struct mp_pool_s) + sizeof(struct mp_node_s));
    if (ret) return NULL;

    p->max = (size < MP_MAX_ALLOC_FROM_POOL) ? size : MP_MAX_ALLOC_FROM_POOL;
    p->current = p->head;
    p->large = NULL;

    p->head->last = (unsigned char *)p + sizeof(struct mp_pool_s) + sizeof(struct mp_node_s);
    p->head->end = p->head->last + size;

    p->head->failed = 0;

    return p;
}

// 释放内存池
void mp_destory_pool(struct mp_pool_s *pool) { 

    struct mp_node_s *h, *n;
    struct mp_large_s *l;

    for (l = pool->large ; l != NULL ; l = l->next) {
        if (l->alloc) {
            free(l->alloc);
        }
    }

    // 从 head->next 开始释放, head 会在 free(pool) 时释放
    h = pool->head->next;

    while (h) {
        n = h->next;
        free(h);
        h = n;
    }

    free(pool);

}

// 重置内存空间
void mp_reset_pool(struct mp_pool_s *pool) { 

    struct mp_node_s *h;
    struct mp_large_s *l;

    for (l = pool->large ; l != NULL ; l = l->next) {
        if (l->alloc) {
            free(l->alloc); // 只是释放了内存空间, 链表结构并未释放
        }
    }

    pool->large = NULL;

    for (h = pool->head ; h != NULL ; h = h->next) {
        h->last = (unsigned char *)h + sizeof(struct mp_node_s);
        // 对这一个小块做标记, 之前的数据还在内存中, 此时申请空间肯定有脏数据
        // 因此每次申请空间时需要注意使用 memset 清理可能的脏数据
    }

}

// 小块内存池的一整块内存申请
static void *mp_alloc_block(struct mp_pool_s *pool, size_t size) {

    unsigned char *m;
    struct mp_node_s *h = pool->head;
    size_t psize = (size_t)(h->end - (unsigned char *)h); // 计算 pool 的大小，即需要分配新的 block 的大小

    int ret = posix_memalign((void **)&m, MP_ALIGNMENT, psize);
    if (ret) return NULL;

    // 初始化新的内存块
    struct mp_node_s *p, *new_node, *current;
    new_node = (struct mp_node_s *)m;

    new_node->end = m + psize;
    new_node->next = NULL;
    new_node->failed = 0;

    // 让 m 指向该块内存结构体之后数据区的起始位置
    m += sizeof(struct mp_node_s); // 跳过一个结构体的位置
    m = mp_align_ptr(m, MP_ALIGNMENT); // 地址对齐
    new_node->last = m + size; // 当前内存为size的数据放入后, 下一次插入即 new_node->last = m + size

    current = pool->current;

    for (p = current ; p->next != NULL ; p = p->next) {
        if (p->failed ++ > 4) {
            current = p->next; // 失败 4 次以上移动 current 指针
        }
    }
    p->next = new_node; // 插入到最后

    pool->current = current ? current : new_node; // 如果是第一次为内存池分配 block, 让 current 指向新分配的 block
    
    return m;
}

// 大块内存申请(需要一个struct mp_large_s指针和一块size的内存)
// mp_large_s指针视为小内存, 存入小内存池中
static void *mp_alloc_large(struct mp_pool_s *pool, size_t size) {

    void *p = malloc(size); // 直接分配内存
    if (p == NULL) return NULL;
 
    /*
    如果已经有之前释放过的指针可以用, 直接使用, 让 alloc 指向刚刚申请的内存即可
    否则在小内存池中开辟一块 sizeof(struct mp_large_s) 的空间
    */
    size_t n = 0;
    struct mp_large_s *large;
    for (large = pool->large; large != NULL ; large = large->next) {
        if (large->alloc == NULL) {
            large->alloc = p;
            return p;
        }
        if (n ++ > 3) break;
    }
    
    /*
    没有释放的, 只能在小内存池中申请
    */
    large = mp_alloc(pool, sizeof(struct mp_large_s));
    if (large == NULL) {
        free(p);
        return NULL;
    }
    /*
    alloc 指向 大块内存空间
    */
    large->alloc = p;
    large->next = pool->large;
    pool->large = large;
 
    return p;

}

// 返回基于一个指定 alignment 的大小为 size 的内存空间，且其地址为 alignment 的整数倍，alignment 为2的幂。
void *mp_memalign(struct mp_pool_s *pool, size_t size, size_t alignment) {
 
    void *p;
     
    int ret = posix_memalign(&p, alignment, size);
    if (ret) {
        return NULL;
    }
 
    struct mp_large_s *large = mp_alloc(pool, sizeof(struct mp_large_s));
    if (large == NULL) {
        free(p);
        return NULL;
    }
 
    large->alloc = p;
    large->next = pool->large;
    pool->large = large;
 
    return p;
}

// 从内存池中分配 size 大小的内存
// mp_alloc: 取得的内存是对齐的
// mp_nalloc: 取得的内存是不考虑对齐的
void *mp_alloc(struct mp_pool_s *pool, size_t size) {
 
    unsigned char *m;
    struct mp_node_s *p;
 
    if (size <= pool->max) { // 小块处理
 
        p = pool->current;
 
        do {
             
            m = mp_align_ptr(p->last, MP_ALIGNMENT);
            if ((size_t)(p->end - m) >= size) { // 如果存在一个 block 能放入 size 大小的内存
                p->last = m + size;
                return m;
            }
            p = p->next;
        } while (p);

        return mp_alloc_block(pool, size); // 都不行就申请一个 block 并且存入 size 的内存
    }
    
    // 大块处理
    return mp_alloc_large(pool, size);
     
}
 
// 从内存池中分配 size 大小的内存, 不考虑内存对齐
void *mp_nalloc(struct mp_pool_s *pool, size_t size) {
 
    unsigned char *m;
    struct mp_node_s *p;
 
    if (size <= pool->max) {
        p = pool->current;
 
        do {
            m = p->last;
            if ((size_t)(p->end - m) >= size) {
                p->last = m+size;
                return m;
            }
            p = p->next;
        } while (p);
 
        return mp_alloc_block(pool, size);
    }
 
    return mp_alloc_large(pool, size);
     
}
 
// 调用 alloc 分配内存后, 进行一次初始化操作
// 同理 C 语言的 malloc 和 calloc
void *mp_calloc(struct mp_pool_s *pool, size_t size) {
 
    void *p = mp_alloc(pool, size);
    if (p) {
        memset(p, 0, size);
    }
 
    return p;
     
}

// 释放大块内存
void mp_free(struct mp_pool_s *pool, void *p) {
 
    struct mp_large_s *l;
    for (l = pool->large; l; l = l->next) {
        if (p == l->alloc) { // 找到指向大块内存的指针
            free(l->alloc);
            l->alloc = NULL;
 
            return ;
        }
    }
     
}
 
 
int main(int argc, char *argv[]) {
 
    int size = 1 << 12;
 
    struct mp_pool_s *p = mp_create_pool(size);
 
    int i = 0;
    for (i = 0;i < 10;i ++) {
 
        void *mp = mp_alloc(p, 512);
//      mp_free(mp);
    }
 
    //printf("mp_create_pool: %ld\n", p->max);
    printf("mp_align(123, 32): %d, mp_align(17, 32): %d\n", mp_align(24, 32), mp_align(17, 32));
    //printf("mp_align_ptr(p->current, 32): %lx, p->current: %lx, mp_align(p->large, 32): %lx, p->large: %lx\n", mp_align_ptr(p->current, 32), p->current, mp_align_ptr(p->large, 32), p->large);
 
    int j = 0;
    for (i = 0;i < 5;i ++) {
 
        char *pp = mp_calloc(p, 32);
        for (j = 0;j < 32;j ++) {
            if (pp[j]) {
                printf("calloc wrong\n");
            }
            printf("calloc success\n");
        }
    }
 
    //printf("mp_reset_pool\n");
 
    for (i = 0;i < 5;i ++) {
        void *l = mp_alloc(p, 8192);
        mp_free(p, l);
    }
 
    mp_reset_pool(p);
 
    //printf("mp_destory_pool\n");
    for (i = 0;i < 58;i ++) {
        mp_alloc(p, 256);
    }
 
    mp_destory_pool(p);
 
    return 0;
 
}