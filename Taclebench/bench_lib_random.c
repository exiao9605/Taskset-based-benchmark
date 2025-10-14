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


// ========== Fragment 1: Binary Search  ==========
void benchmark_fragment1(void) {
    struct DATA { int key, value; } data[15];
    static volatile int seed = 0;
    static volatile uint32_t rand_seed = 12345;
    rand_seed = rand_seed * 1664525 + 1013904223;
    int search_key = rand_seed % 8095;

    // 初始化 data
    for (int i = 0; i < 15; ++i) {
        seed = (seed * 133 + 81) % 8095;
        data[i].key = seed;
        seed = (seed * 133 + 81) % 8095;
        data[i].value = seed;
    }

    // binary search
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

    if (result == -1) __asm__ volatile("" ::: "memory");  // 防止优化
}


// ========== Fragment 2: Bit Count Kernighan  ==========
void benchmark_fragment2(void) {
    static volatile uint32_t seed = 123456789;
    seed = seed * 1664525 + 1013904223;
    uint32_t x = seed;

    int count = 0;
    while (x) {
        x &= (x - 1);
        count++;
    }

    if (count == -1) __asm__ volatile("" ::: "memory");
}


// ========== Fragment 3: Lookup Table Bitcount  ==========
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

    static volatile uint64_t seed = 0xCAFEBABE12345678;
    seed = seed * 1664525 + 1013904223;
    unsigned long x = seed;

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


// ========== Fragment 4: Bitonic Sort  ==========

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

void benchmark_fragment4(void){

    int a[32];
    static volatile uint32_t seed = 0xCAFEBABE;

    for (int i = 0; i < 32; ++i) {
        seed = seed * 1664525 + 1013904223;
        a[i] = seed % 1024;
    }

    bitonic_sort(a, 0, 32, 1);  // ascending sort

    int checksum = a[0] + a[21] + a[31];
    if (checksum == -1)
        __asm__ volatile("" ::: "memory");

}

// ========== Fragment 5: Bubble Sort  ==========

void benchmark_fragment5(void) {
    int array[100];
    int size = 100;
    int sorted = 0;
    int temp;

    // 伪随机初始化：线性同余生成器
    static volatile uint32_t seed = 0xABCD1234;
    for (int i = 0; i < size; ++i) {
        seed = seed * 1664525 + 1013904223;
        array[i] = seed % 1000;  // 限制范围在 0–999
    }

    // 冒泡排序主逻辑
    for (int i = 0; i < size - 1; ++i) {
        sorted = 1;
        for (int j = 0; j < size - 1; ++j) {
            if (j > size - i)
                break;
            if (array[j] > array[j + 1]) {
                temp = array[j];
                array[j] = array[j + 1];
                array[j + 1] = temp;
                sorted = 0;
            }
        }
        if (sorted)
            break;
    }

    // 防止优化：伪校验
    int checksum = array[0] + array[99] + array[42];
    if (checksum == -1)
        __asm__ volatile("" ::: "memory");
}

// ========== Fragment 6: Count Nagetive  ==========
void benchmark_fragment6(void) {
    const int size = 20;
    int array[20][20];
    volatile int seed = 0;

    // 数据填充（简化随机生成器）
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            seed = (seed * 133 + 81) % 8095;
            array[i][j] = seed - 4000;  // 包含负数
        }
    }

    // 求正负数总和与数量
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

    // 防止优化
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

// ========== Fragment 8: matrics 相乘  ==========
void benchmark_fragment8(void) {
    int A[100], B[100], C[100];
    int i, j, k;

    // 初始化 A 和 B（值为 1 可控或伪随机）
    volatile int seed = 0;
    for (i = 0; i < 100; ++i) {
        seed = seed * 133 + 81;
        A[i] = seed % 5;  // 可调值范围 [0–4]
        seed = seed * 133 + 81;
        B[i] = seed % 5;
    }

    // 初始化 C
    for (i = 0; i < 100; ++i)
        C[i] = 0;

    // 执行 10×10 × 10×10 矩阵乘法
    for (k = 0; k < 10; ++k) {
        for (i = 0; i < 10; ++i) {
            int *p_a = &A[i * 10];       // A 行起点
            int *p_b = &B[k * 10];       // B 列转置模拟
            int *p_c = &C[i * 10 + k];   // C[i][k]
            *p_c = 0;
            for (j = 0; j < 10; ++j) {
                *p_c += p_a[j] * p_b[j]; // 实际：A[i][j] * B[j][k]
            }
        }
    }

    // 防止优化：累加校验
    int checksum = 0;
    for (i = 0; i < 100; ++i)
        checksum += C[i];
    if (checksum == -1)
        __asm__ volatile("" ::: "memory");
}
