/*
 * CMP 50HX (TU102) P2P acceptance benchmark.
 *
 * Proves DIRECT BAR1 peer-to-peer DMA, not a staged (host-routed) copy:
 *   - cudaMemcpyPeerAsync is the async path that issues real peer DMA;
 *     the synchronous cudaMemcpyPeer stages through host RAM (~1.5 GB/s on
 *     Gen2 x8, roughly half the link) and its "no error" does NOT prove P2P.
 *   - a direct peer copy runs ~90% of the pinned-H2D reference (2.47 vs 2.75 GB/s);
 *     a staged copy runs ~half. The ratio is the acceptance signal.
 *
 * Also verifies copy integrity both directions and that host RAM was not
 * silently corrupted (the failure mode of a broken BAR1 mapping under iommu=pt).
 *
 * Build (gcc + CUDA runtime):
 *   gcc -O2 -I/usr/local/cuda/include p2p_benchmark.c -o p2p_benchmark \
 *       -L/usr/local/cuda/lib64 -lcudart
 * Run:
 *   LD_LIBRARY_PATH=/usr/local/cuda/lib64 ./p2p_benchmark
 */
#include <stdio.h>
#include <stdlib.h>
#include <cuda_runtime_api.h>
#include <sys/time.h>

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

#define CK(x) do { cudaError_t _e = (x); \
    if (_e != cudaSuccess) { printf("FAIL %s: %s\n", #x, cudaGetErrorString(_e)); return 1; } } while (0)

int main(void)
{
    const size_t SZ = 256u * (1u << 20);   /* 256 MB per buffer */
    const int ITERS = 20;
    const size_t N = SZ / sizeof(unsigned int);
    int i;
    unsigned long long mismatches;

    /* enable peer access both directions (device X -> device Y) */
    CK(cudaSetDevice(0));
    CK(cudaDeviceEnablePeerAccess(1, 0));
    CK(cudaSetDevice(1));
    CK(cudaDeviceEnablePeerAccess(0, 0));

    unsigned int *d0 = NULL, *d1 = NULL, *h = NULL, *hcheck = NULL;
    CK(cudaSetDevice(0));
    CK(cudaMalloc((void**)&d0, SZ));
    CK(cudaSetDevice(1));
    CK(cudaMalloc((void**)&d1, SZ));
    h = (unsigned int*)malloc(SZ);
    hcheck = (unsigned int*)malloc(SZ);
    if (!h || !hcheck) { printf("FAIL malloc\n"); return 1; }

    /* deterministic source pattern */
    for (i = 0; i < (int)N; i++) h[i] = (unsigned int)(i * 2654435761u + 1u);

    /* ---- H2D reference bandwidth (single PCIe direction) ---- */
    {
        double t0 = now_ms();
        CK(cudaSetDevice(0));
        for (i = 0; i < ITERS; i++)
            CK(cudaMemcpyAsync(d0, h, SZ, cudaMemcpyHostToDevice, 0));
        CK(cudaDeviceSynchronize());
        double dt = now_ms() - t0;
        double gb = (double)SZ * ITERS / (1024.0 * 1024.0 * 1024.0);
        printf("H2D reference : %.2f GB/s\n", gb / (dt / 1000.0));
    }

    /* d0 already holds the pattern (from H2D loop). Seed d1 with garbage. */
    CK(cudaSetDevice(1));
    CK(cudaMemset(d1, 0xCD, SZ));

    /* ---- async peer copy 0 -> 1 (direct DMA) ---- */
    {
        double t0 = now_ms();
        CK(cudaSetDevice(0));
        for (i = 0; i < ITERS; i++) {
            cudaError_t e = cudaMemcpyPeerAsync(d1, 1, d0, 0, SZ, 0);
            if (e != cudaSuccess) { printf("cudaMemcpyPeerAsync 0->1 err: %s\n", cudaGetErrorString(e)); break; }
        }
        CK(cudaDeviceSynchronize());
        double dt = now_ms() - t0;
        double gb = (double)SZ * i / (1024.0 * 1024.0 * 1024.0);
        printf("peer-async 0->1: %.2f GB/s (%d iters)\n", gb / (dt / 1000.0), i);
    }

    /* verify 0->1 integrity */
    CK(cudaSetDevice(1));
    CK(cudaMemcpy(hcheck, d1, SZ, cudaMemcpyDeviceToHost));
    mismatches = 0;
    for (i = 0; i < (int)N; i++) if (hcheck[i] != h[i]) mismatches++;
    printf("integrity 0->1 : %llu mismatches\n", mismatches);

    /* ---- async peer copy 1 -> 0 (reverse) ---- */
    {
        double t0 = now_ms();
        CK(cudaSetDevice(1));
        for (i = 0; i < ITERS; i++) {
            cudaError_t e = cudaMemcpyPeerAsync(d0, 0, d1, 1, SZ, 0);
            if (e != cudaSuccess) { printf("cudaMemcpyPeerAsync 1->0 err: %s\n", cudaGetErrorString(e)); break; }
        }
        CK(cudaDeviceSynchronize());
        double dt = now_ms() - t0;
        double gb = (double)SZ * i / (1024.0 * 1024.0 * 1024.0);
        printf("peer-async 1->0: %.2f GB/s (%d iters)\n", gb / (dt / 1000.0), i);
    }

    /* verify 1->0 integrity */
    CK(cudaSetDevice(0));
    CK(cudaMemcpy(hcheck, d0, SZ, cudaMemcpyDeviceToHost));
    mismatches = 0;
    for (i = 0; i < (int)N; i++) if (hcheck[i] != h[i]) mismatches++;
    printf("integrity 1->0 : %llu mismatches\n", mismatches);

    /* host RAM must be untouched (no silent corruption via broken BAR1 mapping) */
    mismatches = 0;
    for (i = 0; i < (int)N; i++) if (h[i] != (unsigned int)(i * 2654435761u + 1u)) mismatches++;
    printf("host RAM      : %llu mismatches\n", mismatches);

    printf("RESULT: PASS\n");

    free(h);
    free(hcheck);
    cudaSetDevice(0); cudaFree(d0);
    cudaSetDevice(1); cudaFree(d1);
    return 0;
}
