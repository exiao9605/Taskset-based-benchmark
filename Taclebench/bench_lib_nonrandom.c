#include <stdint.h>
#include <stdio.h>
#include <limits.h>


void benchmark_fragment0(void); // fragment0: empty loop
void benchmark_fragment1(void); // fragment1: Binary search
void benchmark_fragment2(void); // fragment2: Bit count Kernighan
void benchmark_fragment3(void); // fragment3: Bit count Lookup Table
void benchmark_fragment4(void); // fragment4: Bitonic Sort
void benchmark_fragment5(void); // fragment5: Bubble Sort
void benchmark_fragment6(void); // fragment6: Count nagetive
void benchmark_fragment7(void); // fragment7: Dijkstra 
void benchmark_fragment8(void); // fragment8: matrics x 

// ========== Fragment 0: Empty Loop  ==========
void benchmark_fragment0(void) {
    volatile int sink = 0;
    for (int i = 0; i < 10; ++i) {
        sink += i;  // 防止循环被优化
    }
    if (sink == -1) __asm__ volatile("" ::: "memory");
}


// ======= benchmark_fragment1: Binary Search =======
void benchmark_fragment1(void) {
    struct DATA { int key, value; } data[15];
    int search_key = 40;

    for (int i = 0; i < 15; ++i) {
        data[i].key = i * 10;
        data[i].value = i + 100;
    }

    int low = 0, up = 14, mid, result = -1;
    while (low <= up) {
        mid = (low + up) >> 1;
        if (data[mid].key == search_key) {
            result = data[mid].value;
            break;
        } else if (data[mid].key > search_key) {
            up = mid - 1;
        } else {
            low = mid + 1;
        }
    }

    if (result == -1) __asm__ volatile("" ::: "memory");
}

// ======= benchmark_fragment2: Bit Count (Kernighan) =======
void benchmark_fragment2(void) {
    uint32_t x = 0xF0F0F0F0;
    int count = 0;
    while (x) {
        x &= (x - 1);
        count++;
    }
    if (count == -1) __asm__ volatile("" ::: "memory");
}

// ======= benchmark_fragment3: Bit Count (Lookup Table) =======
void benchmark_fragment3(void) {
    static int bits[256], initialized = 0;
    if (!initialized) {
        for (int i = 0; i < 256; ++i) {
            int v = i, c = 0;
            while (v) { c += v & 1; v >>= 1; }
            bits[i] = c;
        }
        initialized = 1;
    }

    unsigned long x = 0x123456789ABCDEF0;
    int total = 0;
    while (x != 0) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        total += bits[((char*)&x)[sizeof(x) - 1] & 0xFF];
#else
        total += bits[((char*)&x)[0] & 0xFF];
#endif
        x >>= 8;
    }

    if (total == -1) __asm__ volatile("" ::: "memory");
}

// ======= benchmark_fragment4: Bitonic Sort =======

static void bitonic_compare(int *a, int i, int j, int dir) {
    if (dir == (a[i] > a[j])) {
        int tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

static void bitonic_merge(int *a, int lo, int cnt, int dir) {
    if (cnt > 1) {
        int k = cnt / 2;
        for (int i = lo; i < lo + k; ++i)
        bitonic_compare(a, i, i + k, dir);
        bitonic_merge(a, lo, k, dir);
        bitonic_merge(a, lo + k, k, dir);
    }
}

static void bitonic_sort(int *a, int lo, int cnt, int dir) {
    if (cnt > 1) {
        int k = cnt / 2;
        bitonic_sort(a, lo, k, 1);      // ascending
        bitonic_sort(a, lo + k, k, 0);  // descending
        bitonic_merge(a, lo, cnt, dir);
    }
}


void benchmark_fragment4(void) {
    int a[32];
    for (int i = 0; i < 32; ++i)
        a[i] = (31 - i) * 3 % 1024;

    bitonic_sort(a, 0, 32, 1);

    int checksum = a[0] + a[21] + a[31];
    if (checksum == -1) __asm__ volatile("" ::: "memory");
}

// ======= benchmark_fragment5: Bubble Sort =======
void benchmark_fragment5(void) {
    int array[100];
    for (int i = 0; i < 100; ++i)
        array[i] = 100 - i;

    int sorted = 0, temp;
    for (int i = 0; i < 99; ++i) {
        sorted = 1;
        for (int j = 0; j < 99 - i; ++j) {
            if (array[j] > array[j + 1]) {
                temp = array[j];
                array[j] = array[j + 1];
                array[j + 1] = temp;
                sorted = 0;
            }
        }
        if (sorted) break;
    }

    int checksum = array[0] + array[99] + array[42];
    if (checksum == -1) __asm__ volatile("" ::: "memory");
}

// ======= benchmark_fragment6: Count Negative =======
void benchmark_fragment6(void) {
    const int size = 20;
    int array[20][20];

    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j)
            array[i][j] = (i - j) * 10;

    int p_total = 0, p_count = 0;
    int n_total = 0, n_count = 0;

    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            if (array[i][j] >= 0) {
                p_total += array[i][j];
                p_count++;
            } else {
                n_total += array[i][j];
                n_count++;
            }
        }
    }

    if ((p_total + p_count + n_total + n_count) == -999999)
        __asm__ volatile("" ::: "memory");
}


// ========== Fragment 7: Dijkstra算法  ==========
void benchmark_fragment7(void) {
    const int V = 6;  // 顶点数量

    // 邻接矩阵：静态图，边权固定
    int graph[6][6] = {
        {0, 7, 9, 0, 0, 14},
        {7, 0, 10, 15, 0, 0},
        {9, 10, 0, 11, 0, 2},
        {0, 15, 11, 0, 6, 0},
        {0, 0, 0, 6, 0, 9},
        {14, 0, 2, 0, 9, 0}
    };

    int dist[6];       // 最短距离数组
    int visited[6] = {0};  // 访问标记

    // 初始化距离
    for (int i = 0; i < V; ++i)
        dist[i] = INT_MAX;
    dist[0] = 0;  // 源点为 0

    // Dijkstra 核心逻辑
    for (int count = 0; count < V - 1; ++count) {
        int min = INT_MAX, u = -1;
        for (int v = 0; v < V; ++v)
            if (!visited[v] && dist[v] <= min)
                min = dist[v], u = v;

        if (u == -1) break;
        visited[u] = 1;

        for (int v = 0; v < V; ++v) {
            if (!visited[v] && graph[u][v] &&
                dist[u] != INT_MAX &&
                dist[u] + graph[u][v] < dist[v]) {
                dist[v] = dist[u] + graph[u][v];
            }
        }
    }

    // 防止优化
    int checksum = 0;
    for (int i = 0; i < V; ++i) checksum += dist[i];
    if (checksum == -1) __asm__ volatile("" ::: "memory");
}



// ======= benchmark_fragment8: Matrix Multiply =======
void benchmark_fragment8(void) {
    int A[100], B[100], C[100];

    for (int i = 0; i < 100; ++i) {
        A[i] = i % 5;
        B[i] = (i + 2) % 5;
        C[i] = 0;
    }

    for (int k = 0; k < 10; ++k) {
        for (int i = 0; i < 10; ++i) {
            int *p_a = &A[i * 10];
            int *p_b = &B[k * 10];
            int *p_c = &C[i * 10 + k];
            *p_c = 0;
            for (int j = 0; j < 10; ++j) {
                *p_c += p_a[j] * p_b[j];
            }
        }
    }

    int checksum = 0;
    for (int i = 0; i < 100; ++i)
        checksum += C[i];
    if (checksum == -1)
        __asm__ volatile("" ::: "memory");
}