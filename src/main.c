#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096
#define DEFAULT_NUM_FRAMES 3

typedef enum { ALG_FIFO, ALG_LRU, ALG_CLOCK } Algorithm;
typedef enum { WP_WRITE_THROUGH, WP_WRITE_BACK } WritePolicy;

typedef struct {
    int valid;
    unsigned int vpn;
    int frame_index;
    unsigned long last_used; // for TLB LRU
} TLBEntry;

static void print_frames(const int *frames, int n) {
    printf("Frames: [");
    for (int i = 0; i < n; i++) {
        if (frames[i] == -1) printf(" -");
        else printf(" %d", frames[i]);
    }
    printf(" ]\n");
}

static int tlb_lookup(TLBEntry *tlb, int tlb_size, unsigned int vpn,
                      unsigned long tick, int *out_frame) {
    if (!tlb || tlb_size <= 0) return 0;
    for (int i = 0; i < tlb_size; i++) {
        if (tlb[i].valid && tlb[i].vpn == vpn) {
            tlb[i].last_used = tick;
            *out_frame = tlb[i].frame_index;
            return 1; // hit
        }
    }
    return 0; // miss
}

static void tlb_insert(TLBEntry *tlb, int tlb_size, unsigned int vpn,
                       int frame_index, unsigned long tick) {
    if (!tlb || tlb_size <= 0) return;

    // If already there, update it
    for (int i = 0; i < tlb_size; i++) {
        if (tlb[i].valid && tlb[i].vpn == vpn) {
            tlb[i].frame_index = frame_index;
            tlb[i].last_used = tick;
            return;
        }
    }

    // Find empty slot
    for (int i = 0; i < tlb_size; i++) {
        if (!tlb[i].valid) {
            tlb[i].valid = 1;
            tlb[i].vpn = vpn;
            tlb[i].frame_index = frame_index;
            tlb[i].last_used = tick;
            return;
        }
    }

    // Evict LRU entry
    int victim = 0;
    for (int i = 1; i < tlb_size; i++) {
        if (tlb[i].last_used < tlb[victim].last_used) victim = i;
    }
    tlb[victim].valid = 1;
    tlb[victim].vpn = vpn;
    tlb[victim].frame_index = frame_index;
    tlb[victim].last_used = tick;
}

static void tlb_invalidate_vpn(TLBEntry *tlb, int tlb_size, unsigned int vpn) {
    if (!tlb || tlb_size <= 0) return;
    for (int i = 0; i < tlb_size; i++) {
        if (tlb[i].valid && tlb[i].vpn == vpn) {
            tlb[i].valid = 0;
        }
    }
}

static void usage(const char *prog) {
    printf("Usage: %s -a fifo|lru|clock [-f num_frames] [-t tlb_entries] "
           "[-wt | -wb] <tracefile>\n", prog);
}

int main(int argc, char *argv[]) {
    printf("OS Simulator starting...\n");

    Algorithm alg = ALG_FIFO;
    WritePolicy write_policy = WP_WRITE_THROUGH;
    int tlb_size = 0;
    int num_frames = DEFAULT_NUM_FRAMES;
    const char *trace_path = NULL;

    // ---- Parse args ----
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            i++;
            if      (strcmp(argv[i], "fifo")  == 0) alg = ALG_FIFO;
            else if (strcmp(argv[i], "lru")   == 0) alg = ALG_LRU;
            else if (strcmp(argv[i], "clock") == 0) alg = ALG_CLOCK;
            else { usage(argv[0]); return 1; }

        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            i++;
            num_frames = atoi(argv[i]);
            if (num_frames <= 0) {
                fprintf(stderr, "Number of frames must be > 0\n");
                return 1;
            }

        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            i++;
            tlb_size = atoi(argv[i]);
            if (tlb_size < 0) tlb_size = 0;

        } else if (strcmp(argv[i], "-wt") == 0) {
            write_policy = WP_WRITE_THROUGH;

        } else if (strcmp(argv[i], "-wb") == 0) {
            write_policy = WP_WRITE_BACK;

        } else {
            // Must be the trace file
            trace_path = argv[i];
        }
    }

    if (!trace_path) {
        usage(argv[0]);
        return 1;
    }

    FILE *fp = fopen(trace_path, "r");
    if (!fp) {
        perror("Error opening trace file");
        return 1;
    }
    printf("Reading trace file: %s\n", trace_path);

    // ---- Stats ----
    int reads = 0, writes = 0;
    int page_faults = 0;
    int tlb_hits = 0, tlb_misses = 0;
    long long write_backs = 0;   // evictions of dirty pages

    const double TLB_LAT  = 1.0;
    const double MEM_LAT  = 100.0;
    const double DISK_LAT = 10000000.0;

    // ---- Memory frames & metadata ----
    int *frames = (int *)malloc((size_t)num_frames * sizeof(int));
    unsigned long *frame_last_used =
        (unsigned long *)calloc((size_t)num_frames, sizeof(unsigned long));
    int *ref_bits = (int *)calloc((size_t)num_frames, sizeof(int));
    int *dirty    = (int *)calloc((size_t)num_frames, sizeof(int));

    if (!frames || !frame_last_used || !ref_bits || !dirty) {
        perror("Error allocating frame metadata");
        fclose(fp);
        free(frames);
        free(frame_last_used);
        free(ref_bits);
        free(dirty);
        return 1;
    }

    for (int i = 0; i < num_frames; i++) {
        frames[i] = -1;
        frame_last_used[i] = 0;
        ref_bits[i] = 0;
        dirty[i] = 0;
    }

    // FIFO state
    int fifo_index = 0;

    // CLOCK state
    int clock_hand = 0;

    // Tick counter (for LRU timing)
    unsigned long tick = 0;

    // ---- Optional TLB ----
    TLBEntry *tlb = NULL;
    if (tlb_size > 0) {
        tlb = (TLBEntry *)calloc((size_t)tlb_size, sizeof(TLBEntry));
        if (!tlb) {
            perror("Error allocating TLB");
            fclose(fp);
            free(frames);
            free(frame_last_used);
            free(ref_bits);
            free(dirty);
            return 1;
        }
    }

    // ---- Simulation loop ----
    char op;
    unsigned int addr;

    while (fscanf(fp, " %c %x", &op, &addr) == 2) {
        tick++;

        if (op == 'R') reads++;
        else if (op == 'W') writes++;
        else continue; // ignore unknown ops

        unsigned int vpn = addr / PAGE_SIZE;

        // 1) TLB lookup (if enabled)
        int frame_index_from_tlb = -1;
        if (tlb_size > 0) {
            if (tlb_lookup(tlb, tlb_size, vpn, tick, &frame_index_from_tlb)) {
                tlb_hits++;
                printf("Operation: %c | Address: 0x%x | VPN: %u -> TLB HIT (frame %d)\n",
                       op, addr, vpn, frame_index_from_tlb);

                if (frame_index_from_tlb >= 0 && frame_index_from_tlb < num_frames) {
                    if (alg == ALG_LRU) {
                        frame_last_used[frame_index_from_tlb] = tick;
                    }
                    if (alg == ALG_CLOCK) {
                        ref_bits[frame_index_from_tlb] = 1;
                    }
                    if (op == 'W' && write_policy == WP_WRITE_BACK) {
                        dirty[frame_index_from_tlb] = 1;
                    }
                }

                print_frames(frames, num_frames);
                continue;
            } else {
                tlb_misses++;
                printf(" -> TLB MISS\n");
            }
        }

        // 2) Check frames for HIT/MISS
        int hit = 0;
        int hit_frame_index = -1;
        for (int i = 0; i < num_frames; i++) {
            if (frames[i] == (int)vpn) {
                hit = 1;
                hit_frame_index = i;
                break;
            }
        }

        if (hit) {
            printf("Operation: %c | Address: 0x%x | VPN: %u -> HIT\n",
                   op, addr, vpn);

            if (alg == ALG_LRU) {
                frame_last_used[hit_frame_index] = tick;
            }
            if (alg == ALG_CLOCK) {
                ref_bits[hit_frame_index] = 1;
            }
            if (op == 'W' && write_policy == WP_WRITE_BACK) {
                dirty[hit_frame_index] = 1;
            }

            // Put it in TLB (common behavior)
            if (tlb_size > 0) {
                tlb_insert(tlb, tlb_size, vpn, hit_frame_index, tick);
            }

        } else {
            printf("Operation: %c | Address: 0x%x | VPN: %u -> PAGE FAULT\n",
                   op, addr, vpn);
            page_faults++;

            // Choose victim frame
            int victim = -1;

            // If there is an empty frame, use it first
            for (int i = 0; i < num_frames; i++) {
                if (frames[i] == -1) {
                    victim = i;
                    break;
                }
            }

            if (victim == -1) {
                if (alg == ALG_FIFO) {
                    victim = fifo_index;
                    fifo_index = (fifo_index + 1) % num_frames;

                } else if (alg == ALG_LRU) {
                    victim = 0;
                    for (int i = 1; i < num_frames; i++) {
                        if (frame_last_used[i] < frame_last_used[victim]) {
                            victim = i;
                        }
                    }

                } else if (alg == ALG_CLOCK) {
                    while (1) {
                        if (ref_bits[clock_hand] == 0) {
                            victim = clock_hand;
                            clock_hand = (clock_hand + 1) % num_frames;
                            break;
                        }
                        ref_bits[clock_hand] = 0;
                        clock_hand = (clock_hand + 1) % num_frames;
                    }
                }
            }

            // If we evict something, handle TLB + write-back
            if (frames[victim] != -1) {
                if (tlb_size > 0) {
                    tlb_invalidate_vpn(tlb, tlb_size,
                                       (unsigned int)frames[victim]);
                }
                if (write_policy == WP_WRITE_BACK && dirty[victim]) {
                    write_backs++;
                    dirty[victim] = 0;
                }
            }

            frames[victim] = (int)vpn;

            if (alg == ALG_LRU) {
                frame_last_used[victim] = tick;
            }
            if (alg == ALG_CLOCK) {
                ref_bits[victim] = 1;
            }
            if (op == 'W' && write_policy == WP_WRITE_BACK) {
                dirty[victim] = 1;
            }

            // Insert new mapping into TLB
            if (tlb_size > 0) {
                tlb_insert(tlb, tlb_size, vpn, victim, tick);
            }
        }

        print_frames(frames, num_frames);
    }

    fclose(fp);

    // ---- Final stats ----
    printf("\n--- Stats ---\n");
    printf("Algorithm: %s\n",
           (alg == ALG_FIFO) ? "FIFO" :
           (alg == ALG_LRU)  ? "LRU"  : "CLOCK");

    printf("Write policy: %s\n",
           (write_policy == WP_WRITE_THROUGH)
               ? "Write-Through"
               : "Write-Back");

    printf("Frames: %d\n", num_frames);
    printf("Reads: %d\n", reads);
    printf("Writes: %d\n", writes);

    int total_accesses = reads + writes;
    printf("Total accesses: %d\n", total_accesses);
    printf("Total page faults: %d\n", page_faults);

    if (total_accesses > 0) {
        double fault_rate = (double)page_faults / (double)total_accesses;
        double hit_rate   = 1.0 - fault_rate;
        printf("Memory hit rate: %.2f%%\n", hit_rate * 100.0);
        printf("Page fault rate: %.2f%%\n", fault_rate * 100.0);
    }

    if (tlb_size > 0) {
        int tlb_total = tlb_hits + tlb_misses;
        printf("TLB entries: %d\n", tlb_size);
        printf("TLB hits: %d\n", tlb_hits);
        printf("TLB misses: %d\n", tlb_misses);

        if (tlb_total > 0) {
            double tlb_hit_rate = (double)tlb_hits / (double)tlb_total;
            double page_fault_rate =
                (total_accesses > 0)
                    ? (double)page_faults / (double)total_accesses
                    : 0.0;

            double base = tlb_hit_rate * TLB_LAT +
                          (1.0 - tlb_hit_rate) * MEM_LAT;
            double amat = base + page_fault_rate * DISK_LAT;

            printf("TLB hit rate: %.2f%%\n", tlb_hit_rate * 100.0);
            printf("Approx. AMAT: %.2f cycles\n", amat);
        }
    }

    printf("Write-backs (dirty evictions): %lld\n", write_backs);
    printf("Simulation finished.\n");

    free(frames);
    free(frame_last_used);
    free(ref_bits);
    free(dirty);
    free(tlb);

    return 0;
}