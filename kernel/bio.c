// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
#define BLOCK_HASH(blockno) (blockno % NBUCKET)

extern uint ticks;

struct {
  struct spinlock g_lock; // 全局锁
  struct buf buf[NBUF];

  struct spinlock bk_lock[NBUCKET]; // 每个hash bucket都对应有一把锁
  struct buf bucket[NBUCKET];       // hash bucket
  int size; // 缓冲块池（buf[NBUF]）中已使用的块数
} bcache;

void
binit(void)
{
  struct buf *b;

  bcache.size = 0;
  initlock(&bcache.g_lock, "bcache");

  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.bk_lock[i], "bk_lock");
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int bucket_idx = BLOCK_HASH(blockno);

  // 先对blockno对应的bucket上锁即可
  acquire(&bcache.bk_lock[bucket_idx]);

  // Is the block already cached?
  for(b = &bcache.bucket[bucket_idx]; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bk_lock[bucket_idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 在缓存中没有找到，先在缓冲块池中寻找还未分配给bucket的缓存块
  // 需要使用全局锁来保证bcache.size++的原子性
  acquire(&bcache.g_lock);
  if (bcache.size < NBUF) {
    struct buf *b = &bcache.buf[bcache.size++];
    b->next = bcache.bucket[bucket_idx].next;
    bcache.bucket[bucket_idx].next = b;
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    release(&bcache.g_lock);
    // release(&bcache.bk_lock[bucket_idx]);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.g_lock);

  // 在这时才能释放该bucket的锁，假设在检查缓存是否存在后释放：
  // 如果有两个进程1和2，此时缓冲块池还有多个未分配的块，进程1检查bucket，发现没有缓存，释放锁，准备去缓冲块池中拿
  // 此时切换到进程2，进程2检查bucket,发现没有缓存，也去缓冲块池中拿
  // 这样就会导致bucket会添加两个blockno的缓冲块
  release(&bcache.bk_lock[bucket_idx]);

  // 在每个bucket中去找可用的buffer cache
  for (int i = 0; i < NBUCKET; i++) {
    struct buf *cur_buf, *pre_buf, *min_buf, *min_pre_buf;
    uint min_timestamp = -1;

    acquire(&bcache.bk_lock[bucket_idx]);
    pre_buf = &bcache.bucket[bucket_idx];
    cur_buf = pre_buf->next;

    // 遍历bcache.bucket[bucket_idx]
    while (cur_buf) {
      // 为什么这里需要重新检查？考虑这样一种情况：
      // 假设缓冲块池中还有一个未分配的，此时有两个进程，进程1和2都需要访问同一个标号blockno的块
      // 进程1先拿到这个未分配的，并将其放入了对应的bucket中
      // 之后进程2发现池中没有未分配的了，开始遍历所有bucket，
      // 如果这时不重新检查一下blockno对应的bucket，则会导致一个bucket中有两个blockno的块
      if (bucket_idx == BLOCK_HASH(blockno) && cur_buf->blockno == blockno &&
          cur_buf->dev == dev) {
        cur_buf->refcnt++;
        release(&bcache.bk_lock[bucket_idx]);
        acquiresleep(&cur_buf->lock);
        return cur_buf;
      }

      // 只有引用计数为0,并且时间戳最小的缓冲块才可被重新分配
      if (cur_buf->refcnt == 0 && cur_buf->timestamp < min_timestamp) {
        min_pre_buf = pre_buf;
        min_buf = cur_buf;
        min_timestamp = cur_buf->timestamp;
      }
      
      pre_buf = cur_buf;
      cur_buf = cur_buf->next;
    }

    // 在本轮中找到了可重新分配的缓冲块
    if (min_buf) {
      min_buf->dev = dev;
      min_buf->blockno = blockno;
      min_buf->valid = 0;
      min_buf->refcnt = 1;

      // 是自身bucket中的，不用做转移操作
      if (bucket_idx == BLOCK_HASH(blockno)) {
        // release(&bcache.hash_lock);
        release(&bcache.bk_lock[bucket_idx]);
        acquiresleep(&min_buf->lock);
        return min_buf;
      }

      // 是其他bucket中的，需要转移
      // 先将目标bucket中的buffer移除，然后释放锁
      min_pre_buf->next = min_buf->next;
      release(&bcache.bk_lock[bucket_idx]);

      // 接着获取blockno对应的锁，并将从目标bucket中移除的buffer放至blockno对应的bucket，返回该buffer
      bucket_idx = BLOCK_HASH(blockno);
      acquire(&bcache.bk_lock[bucket_idx]);
      min_buf->next = bcache.bucket[bucket_idx].next;
      bcache.bucket[bucket_idx].next = min_buf;
      release(&bcache.bk_lock[bucket_idx]);
      acquiresleep(&min_buf->lock);
      return min_buf;
    }

    release(&bcache.bk_lock[bucket_idx]);
    // 如果到底了，从头来，保证遍历每一个bucket
    if (++bucket_idx == NBUCKET) {
      bucket_idx = 0;
    }
  }
  
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int idx = BLOCK_HASH(b->blockno);
  acquire(&bcache.bk_lock[idx]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->timestamp = ticks;
  }
  
  release(&bcache.bk_lock[idx]);
}

void
bpin(struct buf *b) {
  int idx = BLOCK_HASH(b->blockno);
  acquire(&bcache.bk_lock[idx]);
  b->refcnt++;
  release(&bcache.bk_lock[idx]);
}

void
bunpin(struct buf *b) {
  int idx = BLOCK_HASH(b->blockno);
  acquire(&bcache.bk_lock[idx]);
  b->refcnt--;
  release(&bcache.bk_lock[idx]);
}


