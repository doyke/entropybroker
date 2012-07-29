#define POOL_SIZE	4096	// in bits
#define MAX_EVENT_BITS	11

typedef struct
{
        double last_time;
        double last_delta, last_delta2;
} event_state_t;

class pool
{
private:
	unsigned char entropy_pool[POOL_SIZE / 8];
	unsigned char ivec[8];
	int bits_in_pool;
	event_state_t state;
	bit_count_estimator *bce;

	void update_ivec(void);

public:
	pool(bit_count_estimator *bce);
	pool(int pool_nr, FILE *fh, bit_count_estimator *bce);
	~pool();
	void dump(FILE *fh);

	/* -1 if not full, 0 if full */
	int add_entropy_data(unsigned char *entropy_data, int n_bytes);
	/* returns number of bytes returned, set prng_ok to also return data when pool empty */
	int get_entropy_data(unsigned char *entropy_data, int n_bytes_requested, char prng_ok);
	int get_get_size(void);
	int get_n_bits_in_pool(void);
	int get_pool_size(void);
	bool is_full(void);
	bool is_almost_full(void);
	int add_event(double ts, unsigned char *event_data, int n_event_data);
};
