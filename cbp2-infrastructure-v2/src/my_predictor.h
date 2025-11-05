// my_predictor.h
// This file contains a sample my_predictor class.
// It is a simple 32,768-entry gshare with a history length of 15.

// Replace the sample gshare with a compact TAGE-like predictor.
// Configuration: base bimodal + 4 tagged tables, 64-bit global history.

class my_update : public branch_update {
public:
	// base table index used
	unsigned int base_index;
	// indices looked up in tagged tables
	unsigned int idx[4];
	// which table provided the prediction (-1 means base)
	int provider;
	// which table provided altpred (-1 means base)
	int alt_provider;
	// whether altpred was used
	bool used_alt;
};

class my_predictor : public branch_predictor {
public:
// Parameters for small TAGE configuration (fits in 32kb)
#define MAX_HIST	64
#define BASE_BITS	14			// 16K-entry bimodal
#define NTABLES		4			// number of tagged tables
#define TBL_BITS	10			// 1K entries per tagged table
#define TAG_BITS	8			// 8-bit tags

	my_update u;
	branch_info bi;
	unsigned long long ghist; // 64-bit global history of conditional branches

	// Base predictor: 2-bit counters [0..3]
	unsigned char base[1 << BASE_BITS];

	unsigned short tags[NTABLES][1 << TBL_BITS]; // TAG_BITS used
	unsigned char  ctrs[NTABLES][1 << TBL_BITS]; // 2-bit saturating counters
	unsigned char  us[NTABLES][1 << TBL_BITS];   // 2-bit usefulness counters

	// Per-table history lengths (geometric)
	unsigned int hist_len[NTABLES];

	// Simple aging counter for usefulness
	unsigned int tick;

	my_predictor (void) : ghist(0), tick(0) {
		// Choose geometric history lengths within 64-bit window
		hist_len[0] = 5; // T1 uses 5 history bits
		hist_len[1] = 10; // T2 uses 10 history bits
		hist_len[2] = 20; // T3 uses 20 history bits
		hist_len[3] = 40; // T4 uses 40 history bits
		memset(base, 1, sizeof(base));	// initialize T0 to weakly not taken
		memset(tags, 0, sizeof(tags));	// initialize tags to 0
		memset(ctrs, 1, sizeof(ctrs));	// initialize counters to weakly not taken
		memset(us,   0, sizeof(us));	// initialize usefulness to 0
	}

	inline unsigned int mask_bits(unsigned int n) {
		return (1U << n) - 1U;
	}

	inline unsigned int idx_calc(int t, unsigned int pc) {	// Fold the lower hist_len[t] bits of history into an index
		// mask for table size
		unsigned int m = mask_bits(TBL_BITS);
		// keep global history bits used in the table
		unsigned long long h = ghist & ((hist_len[t] == 64) ? ~0ULL : ((1ULL << hist_len[t]) - 1ULL));
		// Mix history bits
		unsigned long long mix = h ^ (h >> (hist_len[t] ? (hist_len[t] / 2) : 1)) ^ (h * 0x9E3779B97F4A7C15ULL);
		// Combine with PC bits and fold down to 32 bits
		unsigned int x = (unsigned int)(mix) ^ (unsigned int)(mix >> 32) ^ pc ^ (pc >> TBL_BITS);
		// reduce to table size
		return x & m;
	}

	inline unsigned short tag_calc(int t, unsigned int pc) {	// Mix PC and history; keep TAG_BITS
		// mask for table size
		unsigned long long h = ghist & ((hist_len[t] == 64) ? ~0ULL : ((1ULL << hist_len[t]) - 1ULL));
		// Mix history bits with a different constant
		unsigned long long mix = (h ^ (h >> (t + 1)) ^ (h * 0xC6A4A7935BD1E995ULL) ^ ((unsigned long long)pc << 32) ^ pc);
		// Fold down to 32 bits
		unsigned int x = (unsigned int)(mix ^ (mix >> 16));
		// return tag bits
		return (unsigned short)(x & ((1U << TAG_BITS) - 1U));
	}

	inline bool ctr_pred(unsigned char c) { return c >= 2; }
	inline void ctr_inc(unsigned char &c) { if (c < 3) ++c; }
	inline void ctr_dec(unsigned char &c) { if (c > 0) --c; }
	inline bool ctr_weak(unsigned char c) { return c == 1 || c == 2; }

	branch_update *predict (branch_info & b) {
		bi = b;
		u.used_alt = false;	// altpred not used by default
		u.provider = -1;	// base table used by default
		u.alt_provider = -1; // base table used by default

		if (b.br_flags & BR_CONDITIONAL) {
			// Base predictor
			u.base_index = b.address & mask_bits(BASE_BITS);
			bool base_pred = ctr_pred(base[u.base_index]);

			// Probe tagged tables
			int provider = -1;	// base by default
			int alt_provider = -1;	// also base by default
			unsigned char provider_ctr = 0;
			bool alt_pred = base_pred;
			for (int t = NTABLES - 1; t >= 0; --t) {	// start from table w/ longest history
				unsigned int idx = idx_calc(t, b.address);	// calculate index for table
				u.idx[t] = idx;
				unsigned short tg = tag_calc(t, b.address);	// calculate tag for table
				if (tags[t][idx] == tg) {	// update provider if tag match
					if (provider < 0) {
						provider = t;
						provider_ctr = ctrs[t][idx];
					} else if (alt_provider < 0) {
						alt_provider = t;
						alt_pred = ctr_pred(ctrs[t][idx]);
					}
				}
			}

			bool final_pred = base_pred;
			if (provider >= 0) {
				bool ppred = ctr_pred(provider_ctr);
				// Use alternate if provider is weak and alternate exists and differs
				if (ctr_weak(provider_ctr) && alt_provider >= 0) {
					final_pred = alt_pred;
					u.used_alt = (final_pred != ppred);
				} else {
					final_pred = ppred;
					u.used_alt = false;
				}
				u.provider = provider;
				u.alt_provider = alt_provider;
			}

			u.direction_prediction(final_pred);	// update prediction with final_pred
		} else {
			u.direction_prediction(true);	// taken for non-branch instructions
		}

		u.target_prediction(0);
		return &u;
	}

	void update (branch_update *u_in, bool taken, unsigned int target) {
		(void)target;
		if (bi.br_flags & BR_CONDITIONAL) {
			// cast back into my_update to access fields
			my_update *mu = (my_update*)u_in;
			// Always increment base predictor if taken
			if (taken) ctr_inc(base[mu->base_index]);
			// Always decrement base predictor if not taken
			else ctr_dec(base[mu->base_index]);

			// Provider and altpred update
			int provider = mu->provider;
			int alt_provider = mu->alt_provider;
			bool provider_pred = false;
			bool alt_pred = false;
			unsigned char *pctr = 0;
			unsigned char *actr = 0;
			if (alt_provider >= 0) {
				actr = &ctrs[alt_provider][mu->idx[alt_provider]];
				alt_pred = ctr_pred(*actr);
			}
			if (provider >= 0) {
				// update provider counter
				pctr = &ctrs[provider][mu->idx[provider]];
				provider_pred = ctr_pred(*pctr);
				if (taken) ctr_inc(*pctr); else ctr_dec(*pctr);
				// Update usefulness if alternate disagreed (i.e., when used)
				unsigned char &pu = us[provider][mu->idx[provider]];
				if (mu->used_alt) {
					// Reward provider if it would have been correct when overridden; otherwise penalize
					if (provider_pred == taken) { if (pu < 3) ++pu; } else { if (pu > 0) --pu; }
					// Also update alternate provider usefulness if it exists
					if (alt_provider >= 0) {
						unsigned char &au = us[alt_provider][mu->idx[alt_provider]];
						if (alt_pred == taken) { if (au < 3) ++au; } else { if (au > 0) --au; }
					}
				}
			}

			// Allocate on misprediction by provider (or no provider but base wrong)
			bool provider_wrong = (provider >= 0) ? (provider_pred != taken) : false;
			bool base_pred = ctr_pred(base[mu->base_index]);
			if (provider_wrong || (provider < 0 && base_pred != taken)) {
				int allocs = 0;
				int start = (provider < 0) ? 0 : (provider + 1);
				for (int t = start; t < NTABLES && allocs < 2; ++t) {
					unsigned int idx = idx_calc(t, bi.address);
					unsigned short tg = tag_calc(t, bi.address);
					if (tags[t][idx] != tg) {
						// Prefer entries with low usefulness
						if (us[t][idx] == 0) {
							tags[t][idx] = tg;
							ctrs[t][idx] = taken ? 2 : 1; // weak toward outcome
							us[t][idx] = 0;
							++allocs;
						}
					}
				}
				// If couldn't allocate (all useful), try one forced replacement
				if (allocs == 0 && start < NTABLES) {
					int t = start;
					unsigned int idx = idx_calc(t, bi.address);
					unsigned short tg = tag_calc(t, bi.address);
					tags[t][idx] = tg;
					ctrs[t][idx] = taken ? 2 : 1;
					us[t][idx] = 0;
				}
			}

			// Update global history with conditional outcome
			ghist = ((ghist << 1) | (taken ? 1ULL : 0ULL));
			// keep at most 64 bits
			// (mask unnecessary for 64-bit but keeps intent clear)
			ghist &= 0xFFFFFFFFFFFFFFFFULL;

			// Occasionally age usefulness counters (cheap global decay)
			if ((++tick & 0x3FFFF) == 0) { // every ~262k updates
				for (int t = 0; t < NTABLES; ++t) {
					for (unsigned int i = 0; i < (1U << TBL_BITS); ++i) {
						if (us[t][i] > 0) --us[t][i];
					}
				}
			}
		}
	}
};
