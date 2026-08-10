// File-based key-value database (multimap on disk) implemented as a B+ tree.
//
// Records are (index, value) pairs, ordered lexicographically by index and then
// by value.  Because the value participates in the key, a "find index" query is
// a range scan over all records sharing that index, which the leaf-level linked
// list makes sequential.
//
// Only the nodes touched by the current operation are held in memory: a small
// fixed-size buffer pool (clock replacement) sits in front of the data file, so
// resident memory is bounded regardless of how many records are stored.

#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

static const char *DB_FILE = "storage.db";

static const int IDXLEN = 64;   // index strings are at most 64 bytes
#ifndef TUNE_MAXN
#define TUNE_MAXN 60
#endif
#ifndef TUNE_NF
#define TUNE_NF 64
#endif

static const int MAXN = TUNE_MAXN;   // entries per node
static const int MINN = MAXN / 2;

struct Key {
    char idx[IDXLEN];
    int val;
};

// Zero padded, so memcmp over the whole buffer reproduces strcmp order.
static inline int cmpK(const Key &a, const Key &b) {
    int c = memcmp(a.idx, b.idx, IDXLEN);
    if (c) return c < 0 ? -1 : 1;
    if (a.val != b.val) return a.val < b.val ? -1 : 1;
    return 0;
}

// An internal node stores, for each slot i, the smallest key of subtree
// child[i]; a leaf stores the records themselves and links to its successor.
struct Node {
    int isLeaf;
    int count;
    int next;
    int pad;
    Key key[MAXN];
    int child[MAXN];
};

static const int BLK = (int)sizeof(Node);

static const unsigned MAGIC = 0x4B564442u;  // "KVDB"

struct Header {
    unsigned magic;
    int root;      // block of the root node, -1 when the tree is empty
    int nblk;      // highest block index handed out so far
    int freeHead;  // head of the recycled-block list
};

static Header hdr;
static int fd = -1;

/* ------------------------------------------------------------------ */
/* buffer pool                                                        */
/* ------------------------------------------------------------------ */

static const int NF = TUNE_NF;   // frames
static const int HS = 2048;      // hash buckets

struct Frame {
    int pos, pin, dirty, ref, hnext;
    Node nd;
};

static Frame FR[NF];
static int HT[HS];
static int hand = 0;

static inline int hfun(int p) {
    return (int)(((unsigned)p * 2654435761u) >> 16) & (HS - 1);
}

static void hins(int f) {
    int b = hfun(FR[f].pos);
    FR[f].hnext = HT[b];
    HT[b] = f;
}

static void hdel(int f) {
    int b = hfun(FR[f].pos);
    int *pp = &HT[b];
    while (*pp != -1) {
        if (*pp == f) { *pp = FR[f].hnext; return; }
        pp = &FR[*pp].hnext;
    }
}

static void writeBack(int f) {
    if (FR[f].dirty) {
        (void)!pwrite(fd, &FR[f].nd, BLK, (off_t)FR[f].pos * BLK);
        FR[f].dirty = 0;
    }
}

static int victim() {
    for (;;) {
        int i = hand;
        hand = (hand + 1 == NF) ? 0 : hand + 1;
        if (FR[i].pin) continue;
        if (FR[i].ref) { FR[i].ref = 0; continue; }
        return i;
    }
}

// Returns a pinned frame holding block `pos`.  `doRead` is 0 for freshly
// allocated blocks, whose on-disk contents are meaningless.
static int fetch(int pos, int doRead) {
    int b = hfun(pos);
    for (int i = HT[b]; i != -1; i = FR[i].hnext) {
        if (FR[i].pos == pos) { FR[i].pin++; FR[i].ref = 1; return i; }
    }
    int v = victim();
    if (FR[v].pos != -1) { writeBack(v); hdel(v); }
    FR[v].pos = pos;
    FR[v].dirty = 0;
    FR[v].ref = 1;
    FR[v].pin = 1;
    hins(v);
    if (doRead) {
        if (pread(fd, &FR[v].nd, BLK, (off_t)pos * BLK) != BLK)
            memset(&FR[v].nd, 0, sizeof(Node));
    } else {
        memset(&FR[v].nd, 0, sizeof(Node));
    }
    return v;
}

static inline void unpin(int f) { FR[f].pin--; }

static int allocNode() {
    if (hdr.freeHead != -1) {
        int p = hdr.freeHead;
        int f = fetch(p, 1);
        hdr.freeHead = FR[f].nd.next;
        memset(&FR[f].nd, 0, sizeof(Node));
        FR[f].dirty = 1;
        return f;
    }
    int f = fetch(++hdr.nblk, 0);
    FR[f].dirty = 1;
    return f;
}

static void freeNode(int f) {
    FR[f].nd.next = hdr.freeHead;
    hdr.freeHead = FR[f].pos;
    FR[f].dirty = 1;
}

static void openDB() {
    fd = open(DB_FILE, O_RDWR);
    bool fresh = false;
    if (fd < 0) {
        fd = open(DB_FILE, O_RDWR | O_CREAT, 0644);
        if (fd < 0) exit(1);
        fresh = true;
    } else if (pread(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr) ||
               hdr.magic != MAGIC) {
        fresh = true;
    }
    if (fresh) {
        hdr.magic = MAGIC;
        hdr.root = -1;
        hdr.nblk = 0;
        hdr.freeHead = -1;
    }
    for (int i = 0; i < NF; i++) FR[i].pos = -1;
    for (int i = 0; i < HS; i++) HT[i] = -1;
}

static void closeDB() {
    for (int i = 0; i < NF; i++)
        if (FR[i].pos != -1) writeBack(i);
    (void)!pwrite(fd, &hdr, sizeof(hdr), 0);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* tree operations                                                    */
/* ------------------------------------------------------------------ */

// Last slot whose separator is <= k (slot 0 when k precedes everything).
static inline int childIdx(const Node &nd, const Key &k) {
    int lo = 0, hi = nd.count - 1, res = 0;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (cmpK(nd.key[m], k) <= 0) { res = m; lo = m + 1; }
        else hi = m - 1;
    }
    return res;
}

static inline int lowerBound(const Node &nd, const Key &k) {
    int lo = 0, hi = nd.count;
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        if (cmpK(nd.key[m], k) < 0) lo = m + 1; else hi = m;
    }
    return lo;
}

// Moves the upper half of the full node in frame `f` into a new block.
// Returns that block and reports its smallest key through `sep`.
static int splitNode(int f, Key &sep) {
    int nf = allocNode();
    Node &L = FR[f].nd, &R = FR[nf].nd;
    int half = MAXN / 2;
    R.isLeaf = L.isLeaf;
    R.count = MAXN - half;
    memcpy(R.key, L.key + half, sizeof(Key) * R.count);
    memcpy(R.child, L.child + half, sizeof(int) * R.count);
    if (L.isLeaf) { R.next = L.next; L.next = FR[nf].pos; }
    else R.next = -1;
    L.count = half;
    FR[f].dirty = 1;
    sep = R.key[0];
    int rp = FR[nf].pos;
    unpin(nf);
    return rp;
}

// Splits every full node on the way down, so the destination leaf always has
// room and the parent always has room for the promoted separator.
static void doInsert(const Key &k) {
    if (hdr.root == -1) {
        int f = allocNode();
        FR[f].nd.isLeaf = 1;
        FR[f].nd.count = 1;
        FR[f].nd.next = -1;
        FR[f].nd.key[0] = k;
        hdr.root = FR[f].pos;
        unpin(f);
        return;
    }

    int rf = fetch(hdr.root, 1);
    if (FR[rf].nd.count == MAXN) {
        Key sep;
        int lp = FR[rf].pos;
        Key lk = FR[rf].nd.key[0];
        int rp = splitNode(rf, sep);
        int nrf = allocNode();
        Node &NR = FR[nrf].nd;
        NR.isLeaf = 0;
        NR.count = 2;
        NR.next = -1;
        NR.key[0] = lk;  NR.child[0] = lp;
        NR.key[1] = sep; NR.child[1] = rp;
        hdr.root = FR[nrf].pos;
        unpin(nrf);
    }
    unpin(rf);

    int cur = hdr.root;
    for (;;) {
        int f = fetch(cur, 1);
        Node *nd = &FR[f].nd;
        if (nd->isLeaf) {
            int lo = lowerBound(*nd, k);
            if (lo < nd->count && cmpK(nd->key[lo], k) == 0) { unpin(f); return; }
            for (int j = nd->count; j > lo; j--) nd->key[j] = nd->key[j - 1];
            nd->key[lo] = k;
            nd->count++;
            FR[f].dirty = 1;
            unpin(f);
            return;
        }
        // Keep the leftmost separator equal to the subtree minimum.
        if (cmpK(k, nd->key[0]) < 0) { nd->key[0] = k; FR[f].dirty = 1; }
        int i = childIdx(*nd, k);
        int c = nd->child[i];
        int cf = fetch(c, 1);
        if (FR[cf].nd.count == MAXN) {
            Key sep;
            int rp = splitNode(cf, sep);
            for (int j = nd->count; j > i + 1; j--) {
                nd->key[j] = nd->key[j - 1];
                nd->child[j] = nd->child[j - 1];
            }
            nd->key[i + 1] = sep;
            nd->child[i + 1] = rp;
            nd->count++;
            FR[f].dirty = 1;
            if (cmpK(k, sep) >= 0) c = rp;
        }
        unpin(cf);
        unpin(f);
        cur = c;
    }
}

// Borrows or merges on the way down so the node we finally delete from can
// afford to lose an entry.  A separator that is smaller than its subtree's true
// minimum is still a valid separator, so removals never propagate key updates.
static void doDelete(const Key &k) {
    if (hdr.root == -1) return;
    int cur = hdr.root;
    for (;;) {
        int f = fetch(cur, 1);
        Node *nd = &FR[f].nd;
        if (nd->isLeaf) {
            int lo = lowerBound(*nd, k);
            if (lo < nd->count && cmpK(nd->key[lo], k) == 0) {
                for (int j = lo; j + 1 < nd->count; j++) nd->key[j] = nd->key[j + 1];
                nd->count--;
                FR[f].dirty = 1;
            }
            unpin(f);
            return;
        }
        int i = childIdx(*nd, k);
        int c = nd->child[i];
        int cf = fetch(c, 1);
        if (FR[cf].nd.count <= MINN) {
            int done = 0;
            if (i > 0) {
                int lf = fetch(nd->child[i - 1], 1);
                Node &L = FR[lf].nd, &C = FR[cf].nd;
                if (L.count > MINN) {
                    for (int j = C.count; j > 0; j--) {
                        C.key[j] = C.key[j - 1];
                        C.child[j] = C.child[j - 1];
                    }
                    C.key[0] = L.key[L.count - 1];
                    C.child[0] = L.child[L.count - 1];
                    C.count++;
                    L.count--;
                    nd->key[i] = C.key[0];
                    FR[lf].dirty = FR[cf].dirty = FR[f].dirty = 1;
                    done = 1;
                }
                unpin(lf);
            }
            if (!done && i + 1 < nd->count) {
                int rf = fetch(nd->child[i + 1], 1);
                Node &R = FR[rf].nd, &C = FR[cf].nd;
                if (R.count > MINN) {
                    C.key[C.count] = R.key[0];
                    C.child[C.count] = R.child[0];
                    C.count++;
                    for (int j = 0; j + 1 < R.count; j++) {
                        R.key[j] = R.key[j + 1];
                        R.child[j] = R.child[j + 1];
                    }
                    R.count--;
                    nd->key[i + 1] = R.key[0];
                    FR[rf].dirty = FR[cf].dirty = FR[f].dirty = 1;
                    done = 1;
                }
                unpin(rf);
            }
            if (!done) {
                if (i > 0) {
                    int lf = fetch(nd->child[i - 1], 1);
                    Node &L = FR[lf].nd, &C = FR[cf].nd;
                    memcpy(L.key + L.count, C.key, sizeof(Key) * C.count);
                    memcpy(L.child + L.count, C.child, sizeof(int) * C.count);
                    L.count += C.count;
                    if (L.isLeaf) L.next = C.next;
                    FR[lf].dirty = 1;
                    freeNode(cf);
                    unpin(cf);
                    for (int j = i; j + 1 < nd->count; j++) {
                        nd->key[j] = nd->key[j + 1];
                        nd->child[j] = nd->child[j + 1];
                    }
                    nd->count--;
                    FR[f].dirty = 1;
                    c = FR[lf].pos;
                    cf = lf;
                } else {
                    int rf = fetch(nd->child[i + 1], 1);
                    Node &R = FR[rf].nd, &C = FR[cf].nd;
                    memcpy(C.key + C.count, R.key, sizeof(Key) * R.count);
                    memcpy(C.child + C.count, R.child, sizeof(int) * R.count);
                    C.count += R.count;
                    if (C.isLeaf) C.next = R.next;
                    FR[cf].dirty = 1;
                    freeNode(rf);
                    unpin(rf);
                    for (int j = i + 1; j + 1 < nd->count; j++) {
                        nd->key[j] = nd->key[j + 1];
                        nd->child[j] = nd->child[j + 1];
                    }
                    nd->count--;
                    FR[f].dirty = 1;
                }
                if (FR[f].pos == hdr.root && nd->count == 1) {
                    hdr.root = nd->child[0];
                    freeNode(f);
                }
            }
        }
        unpin(cf);
        unpin(f);
        cur = c;
    }
}

/* ------------------------------------------------------------------ */
/* buffered stdio                                                     */
/* ------------------------------------------------------------------ */

static char ibuf[1 << 14];
static int ilen = 0, ipos = 0;

static inline int gc() {
    if (ipos == ilen) {
        ilen = (int)read(0, ibuf, sizeof(ibuf));
        ipos = 0;
        if (ilen <= 0) return -1;
    }
    return (unsigned char)ibuf[ipos++];
}

static char obuf[1 << 15];
static int olen = 0;

static void flushOut() {
    int off = 0;
    while (off < olen) {
        ssize_t w = write(1, obuf + off, olen - off);
        if (w <= 0) break;
        off += (int)w;
    }
    olen = 0;
}

static inline void put(char c) {
    if (olen == (int)sizeof(obuf)) flushOut();
    obuf[olen++] = c;
}

static void putUInt(unsigned v) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put(t[--n]);
}

// Reads a whitespace-delimited token; returns its length, or -1 at EOF.
static int readTok(char *dst, int cap) {
    int c = gc();
    while (c == ' ' || c == '\n' || c == '\r' || c == '\t') c = gc();
    if (c < 0) return -1;
    int n = 0;
    while (c > ' ') {
        if (n < cap) dst[n] = (char)c;
        n++;
        c = gc();
    }
    return n;
}

/* ------------------------------------------------------------------ */

int main() {
    openDB();

    char tok[256];
    // The first token is the command count.  If it turns out to be a command
    // instead, fall back to reading until end of input.
    long long n = 0;
    bool pending = false;
    int len = readTok(tok, sizeof(tok));
    if (len <= 0) { closeDB(); return 0; }
    if (tok[0] >= '0' && tok[0] <= '9') {
        for (int i = 0; i < len && tok[i] >= '0' && tok[i] <= '9'; i++)
            n = n * 10 + (tok[i] - '0');
    } else {
        n = 0x7fffffffLL;
        pending = true;
    }

    Key k;
    for (long long q = 0; q < n; q++) {
        if (pending) pending = false;
        else if ((len = readTok(tok, sizeof(tok))) < 0) break;
        char op = tok[0];

        memset(k.idx, 0, IDXLEN);
        if (readTok(k.idx, IDXLEN) < 0) break;

        if (op == 'f') {
            k.val = 0;  // values are non-negative, so this is the range start
            if (hdr.root == -1) { put('n'); put('u'); put('l'); put('l'); put('\n'); continue; }
            int cur = hdr.root;
            for (;;) {
                int f = fetch(cur, 1);
                if (FR[f].nd.isLeaf) { unpin(f); break; }
                int i = childIdx(FR[f].nd, k);
                int c = FR[f].nd.child[i];
                unpin(f);
                cur = c;
            }
            bool any = false;
            int f = fetch(cur, 1);
            int p = lowerBound(FR[f].nd, k);
            for (;;) {
                Node &nd = FR[f].nd;
                bool stop = false;
                while (p < nd.count) {
                    if (memcmp(nd.key[p].idx, k.idx, IDXLEN) != 0) { stop = true; break; }
                    if (any) put(' ');
                    putUInt((unsigned)nd.key[p].val);
                    any = true;
                    p++;
                }
                int nxt = nd.next;
                unpin(f);
                if (stop || nxt == -1) break;
                f = fetch(nxt, 1);
                p = 0;
            }
            if (!any) { put('n'); put('u'); put('l'); put('l'); }
            put('\n');
        } else {
            int vlen = readTok(tok, sizeof(tok));
            if (vlen < 0) break;
            unsigned v = 0;
            for (int i = 0; i < vlen && tok[i] >= '0' && tok[i] <= '9'; i++)
                v = v * 10u + (unsigned)(tok[i] - '0');
            k.val = (int)v;
            if (op == 'i') doInsert(k);
            else doDelete(k);
        }
    }

    flushOut();
    closeDB();
    return 0;
}
