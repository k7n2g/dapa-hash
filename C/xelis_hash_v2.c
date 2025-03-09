#define _GNU_SOURCE
#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <wmmintrin.h>
#include "BLAKE3/c/blake3.h"
#include "ChaCha20-SIMD/chacha20.h"

#define INPUT_LEN (112)
#define MEMSIZE (429 * 128)
#define ITERS (3)
#define HASHSIZE (32)

static inline void blake3(const uint8_t *input, int len, uint8_t *output)
{
	blake3_hasher hasher;
	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, input, len);
	blake3_hasher_finalize(&hasher, output, BLAKE3_OUT_LEN);
}

#define HASH_SIZE (32)
#define CHUNK_SIZE (32)
#define NONCE_SIZE (12)
#define OUTPUT_SIZE (MEMSIZE * 8)
#define CHUNKS (4)
#define INPUT_LEN (112)

void stage1(const uint8_t *input, size_t input_len, uint8_t scratch_pad[OUTPUT_SIZE])
{
	uint8_t key[CHUNK_SIZE * CHUNKS] = {0};
	uint8_t input_hash[HASH_SIZE];
	uint8_t buffer[CHUNK_SIZE * 2];
	memcpy(key, input, INPUT_LEN);
	blake3(input, INPUT_LEN, buffer);

	uint8_t *t = scratch_pad;

	memcpy(buffer + CHUNK_SIZE, key + 0 * CHUNK_SIZE, CHUNK_SIZE);
	blake3(buffer, CHUNK_SIZE * 2, input_hash);
	chacha_encrypt(input_hash, buffer, NULL, t, OUTPUT_SIZE / CHUNKS, 8);

	t += OUTPUT_SIZE / CHUNKS;
	memcpy(buffer, input_hash, CHUNK_SIZE);
	memcpy(buffer + CHUNK_SIZE, key + 1 * CHUNK_SIZE, CHUNK_SIZE);
	blake3(buffer, CHUNK_SIZE * 2, input_hash);
	chacha_encrypt(input_hash, t - NONCE_SIZE, NULL, t, OUTPUT_SIZE / CHUNKS, 8);

	t += OUTPUT_SIZE / CHUNKS;
	memcpy(buffer, input_hash, CHUNK_SIZE);
	memcpy(buffer + CHUNK_SIZE, key + 2 * CHUNK_SIZE, CHUNK_SIZE);
	blake3(buffer, CHUNK_SIZE * 2, input_hash);
	chacha_encrypt(input_hash, t - NONCE_SIZE, NULL, t, OUTPUT_SIZE / CHUNKS, 8);

	t += OUTPUT_SIZE / CHUNKS;
	memcpy(buffer, input_hash, CHUNK_SIZE);
	memcpy(buffer + CHUNK_SIZE, key + 3 * CHUNK_SIZE, CHUNK_SIZE);
	blake3(buffer, CHUNK_SIZE * 2, input_hash);
	chacha_encrypt(input_hash, t - NONCE_SIZE, NULL, t, OUTPUT_SIZE / CHUNKS, 8);
}

#define KEY "xelishash-pow-v2"
#define BUFSIZE (MEMSIZE / 2)

// https://danlark.org/2020/06/14/128-bit-division
static inline uint64_t Divide128Div64To64(uint64_t high, uint64_t low, uint64_t divisor, uint64_t *remainder)
{
	uint64_t result;
	__asm__("divq %[v]"
			: "=a"(result), "=d"(*remainder) // Output parametrs, =a for rax, =d for rdx, [v] is an
			// alias for divisor, input paramters "a" and "d" for low and high.
			: [v] "r"(divisor), "a"(low), "d"(high));
	return result;
}

static inline uint64_t udiv(uint64_t high, uint64_t low, uint64_t divisor)
{
	uint64_t remainder;

	if (high < divisor)
	{
		return Divide128Div64To64(high, low, divisor, &remainder);
	}
	else
	{
		uint64_t qhi = Divide128Div64To64(0, high, divisor, &high);
		return Divide128Div64To64(high, low, divisor, &remainder);
	}
}

static inline uint64_t ROTR(uint64_t x, uint32_t r)
{
	asm("rorq %%cl, %0" : "+r"(x) : "c"(r));
	return x;
}

static inline uint64_t ROTL(uint64_t x, uint32_t r)
{
	asm("rolq %%cl, %0" : "+r"(x) : "c"(r));
	return x;
}

static inline __uint128_t combine_uint64(uint64_t high, uint64_t low)
{
	return ((__uint128_t)high << 64) | low;
}

/*
uint64_t isqrt(uint64_t n) {
	if (n < 2)
		return n;

	uint64_t x = n;
	uint64_t y = (x + 1) >> 1;

	while (y < x) {
		x = y;
		y = (x + n / x) >> 1;
	}

	return x;
}
*/

uint64_t isqrt(uint64_t n)
{
	if (n < 2)
		return n;

	uint64_t x = n;
	uint64_t result = 0;
	uint64_t bit = (uint64_t)1 << 62; // The second-to-top bit is set

	// "bit" starts at the highest power of four <= the argument.
	while (bit > x)
		bit >>= 2;

	while (bit != 0)
	{
		if (x >= result + bit)
		{
			x -= result + bit;
			result = (result >> 1) + bit;
		}
		else
		{
			result >>= 1;
		}
		bit >>= 2;
	}

	return result;
}

void static inline uint64_to_le_bytes(uint64_t value, uint8_t *bytes)
{
	for (int i = 0; i < 8; i++)
	{
		bytes[i] = value & 0xFF;
		value >>= 8;
	}
}

uint64_t static inline le_bytes_to_uint64(const uint8_t *bytes)
{
	uint64_t value = 0;
	for (int i = 7; i >= 0; i--)
		value = (value << 8) | bytes[i];
	return value;
}

void static inline aes_single_round(uint8_t *block, const uint8_t *key)
{
	__m128i block_vec = _mm_loadu_si128((const __m128i *)block);
	__m128i key_vec = _mm_loadu_si128((const __m128i *)key);

	// Perform single AES encryption round
	block_vec = _mm_aesenc_si128(block_vec, key_vec);

	_mm_storeu_si128((__m128i *)block, block_vec);
}

void stage3(uint64_t *scratch)
{
	uint64_t *mem_buffer_a = scratch;
	uint64_t *mem_buffer_b = &scratch[BUFSIZE];

	uint64_t addr_a = mem_buffer_b[BUFSIZE - 1];
	uint64_t addr_b = mem_buffer_a[BUFSIZE - 1] >> 32;
	uint32_t r = 0;

	for (uint32_t i = 0; i < ITERS; i++)
	{
		uint64_t mem_a = mem_buffer_a[addr_a % BUFSIZE];
		uint64_t mem_b = mem_buffer_b[addr_b % BUFSIZE];

		uint8_t block[16];
		uint64_to_le_bytes(mem_b, block);
		uint64_to_le_bytes(mem_a, block + 8);
		aes_single_round(block, KEY);

		uint64_t hash1 = le_bytes_to_uint64(block);
		uint64_t hash2 = mem_a ^ mem_b;
		uint64_t result = ~(hash1 ^ hash2);

		for (uint32_t j = 0; j < BUFSIZE; j++)
		{
			uint64_t a = mem_buffer_a[result % BUFSIZE];
			uint64_t b = mem_buffer_b[~ROTR(result, r) % BUFSIZE];
			uint64_t c = (r < BUFSIZE) ? mem_buffer_a[r] : mem_buffer_b[r - BUFSIZE];
			r = (r < MEMSIZE - 1) ? r + 1 : 0;

			uint64_t v;
			__uint128_t t1, t2;
			switch (ROTL(result, (uint32_t)c) & 0xf)
			{
			case 0:
				v = ROTL(c, i * j) ^ b;
				break;
			case 1:
				v = ROTR(c, i * j) ^ a;
				break;
			case 2:
				v = a ^ b ^ c;
				break;
			case 3:
				v = ((a + b) * c);
				break;
			case 4:
				v = ((b - c) * a);
				break;
			case 5:
				v = (c - a + b);
				break;
			case 6:
				v = (a - b + c);
				break;
			case 7:
				v = (b * c + a);
				break;
			case 8:
				v = (c * a + b);
				break;
			case 9:
				v = (a * b * c);
				break;
			case 10:
			{
				t1 = combine_uint64(a, b);
				uint64_t t2 = c | 1;
				v = t1 % t2;
			}
			break;
			case 11:
			{
				t1 = combine_uint64(b, c);
				t2 = combine_uint64(ROTL(result, r), a | 2);
				v = (t2 > t1) ? c : t1 % t2;
			}
			break;
			case 12:
				v = udiv(c, a, b | 4);
				break;
			case 13:
			{
				t1 = combine_uint64(ROTL(result, r), b);
				t2 = combine_uint64(a, c | 8);
				v = (t1 > t2) ? t1 / t2 : a ^ b;
			}
			break;
			case 14:
			{
				t1 = combine_uint64(b, a);
				uint64_t t2 = c;
				v = (t1 * t2) >> 64;
			}
			break;
			case 15:
			{
				t1 = combine_uint64(a, c);
				t2 = combine_uint64(ROTR(result, r), b);
				v = (t1 * t2) >> 64;
			}
			break;
			}
			result = ROTL(result ^ v, 1);

			uint64_t t = mem_buffer_a[BUFSIZE - j - 1] ^ result;
			mem_buffer_a[BUFSIZE - j - 1] = t;
			mem_buffer_b[j] ^= ROTR(t, result);
		}
		addr_a = result;
		addr_b = isqrt(result);
	}
}

int dapa_hash_v2_init()
{
	// return sodium_init();
}

void dapa_hash_v2(uint8_t in[INPUT_LEN], uint8_t hash[HASHSIZE], uint64_t scratch[MEMSIZE])
{
	uint8_t *scratch_uint8 = (uint8_t *)scratch;

	stage1(in, INPUT_LEN, scratch_uint8);
	stage3(scratch);
	blake3(scratch_uint8, OUTPUT_SIZE, hash);
}

double display_time(const char *stage, struct timespec start, struct timespec end, int iterations)
{
	uint64_t total_time = (end.tv_sec - start.tv_sec) * 1000000000ULL + (end.tv_nsec - start.tv_nsec);
	double time_per = (double)total_time / iterations;
	printf("%s: %.3f ms\n", stage, time_per / 1000000.0);
	return time_per;
}

void timing_test(int N)
{
	uint8_t hash[HASHSIZE];
	struct timespec start, end;
	double time_per, time_sum = 0;

	uint8_t *input = (uint8_t *)calloc(INPUT_LEN, sizeof(uint8_t));
	uint64_t *scratch = (uint64_t *)calloc(MEMSIZE, sizeof(uint64_t));
	uint8_t *scratch_uint8 = (uint8_t *)scratch;

	dapa_hash_v2_init();

	printf("Timing:\n");
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < N; i++)
		stage1(input, INPUT_LEN, scratch_uint8);
	clock_gettime(CLOCK_MONOTONIC, &end);
	time_sum += display_time("stage1", start, end, N);

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < N; i++)
		stage3(scratch);
	clock_gettime(CLOCK_MONOTONIC, &end);
	time_sum += display_time("stage3", start, end, N);

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < N; i++)
		blake3(scratch_uint8, OUTPUT_SIZE, hash);
	clock_gettime(CLOCK_MONOTONIC, &end);
	time_sum += display_time("stage4", start, end, N);

	printf("Total:  %.3f ms (%d avg)\n", time_sum / 1000000.0, N);

	// verify output
	uint8_t gold[HASHSIZE] = {
		126, 219, 112, 240, 116, 133, 115,
		144, 39, 40, 164, 105, 30, 158, 45,
		126, 64, 67, 238, 52, 200, 35, 161, 19,
		144, 211, 214, 225, 95, 190, 146, 27};

	dapa_hash_v2(input, hash, scratch);
	if (memcmp(gold, hash, HASHSIZE))
		printf("Failed!\n");
	else
		printf("Passed!\n");

	free(input);
	free(scratch);
}

typedef struct
{
	int thread_id;
	int iterations;
	uint8_t *input;
	uint64_t *scratch;
	uint8_t *hash;
} thread_data_t;

void set_thread_affinity(int thread_id)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(thread_id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
	int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	if (rc != 0)
	{
		fprintf(stderr, "Error: Unable to set CPU affinity for thread %d\n", thread_id);
	}
}

void *hash_thread(void *arg)
{
	thread_data_t *data = (thread_data_t *)arg;
	//	set_thread_affinity(data->thread_id);

	for (int i = 0; i < data->iterations; ++i)
		dapa_hash_v2(data->input, data->hash, data->scratch);

	pthread_exit(NULL);
}

void hash_test(int t, int i)
{
	pthread_t *threads;
	thread_data_t *thread_data;

	dapa_hash_v2_init();

	printf("\n%-10s %-15s %-10s\n", "Threads", "Hashes", "Hash/s");
	for (int tc = 1; tc <= t; ++tc)
	{
		threads = (pthread_t *)malloc(tc * sizeof(pthread_t));
		thread_data = (thread_data_t *)malloc(tc * sizeof(thread_data_t));
		struct timespec start, end;

		clock_gettime(CLOCK_REALTIME, &start);
		for (int j = 0; j < tc; ++j)
		{
			thread_data[j].thread_id = j;
			thread_data[j].iterations = i;
			thread_data[j].input = (uint8_t *)calloc(INPUT_LEN, sizeof(uint8_t));
			thread_data[j].scratch = (uint64_t *)calloc(MEMSIZE, sizeof(uint64_t));
			thread_data[j].hash = (uint8_t *)calloc(HASHSIZE, sizeof(uint8_t));
			pthread_create(&threads[j], NULL, hash_thread, (void *)&thread_data[j]);
		}

		for (int j = 0; j < tc; ++j)
		{
			pthread_join(threads[j], NULL);
			free(thread_data[j].input);
			free(thread_data[j].scratch);
			free(thread_data[j].hash);
		}

		clock_gettime(CLOCK_REALTIME, &end);

		double time_taken = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
		double hashes_per_second = (double)(tc * i) / time_taken;
		printf("%-10d %-15d %-10.2f\n", tc, i * tc, hashes_per_second);

		free(threads);
		free(thread_data);
	}
}

void print_usage(const char *prog_name)
{
	printf("Usage: %s [-n iterations] [-t threads]\n", prog_name);
	printf("  -n iterations    Number of iterations for tests\n");
	printf("  -t threads       Number of threads to test\n");
	printf("  -h               Show this help message\n");
}

int main(int argc, char *argv[])
{
	int N = 1000, T = 8;
	int opt;

	while ((opt = getopt(argc, argv, "n:t:h")) != -1)
	{
		switch (opt)
		{
		case 'n':
			N = atoi(optarg);
			break;
		case 't':
			T = atoi(optarg);
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	timing_test(N);
	hash_test(T, N);
}
