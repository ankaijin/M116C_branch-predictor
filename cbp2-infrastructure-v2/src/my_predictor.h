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
	unsigned int idx[14];
	// which table provided the prediction (-1 means base)
	int provider;
	// which table provided the alternate prediction (-1 means none/base)
	int alt_provider;
	// whether altpred was used
	bool used_alt;
};

class my_predictor : public branch_predictor {
public:
// Preset A: L-TAGE heavy (~1MB). 64K-entry base + 14 tagged tables with geometric histories up to ~800.
#define HLEN		1024			// number of history bits kept (power of two for fast wrap)
#define BASE_BITS	16			// 64K-entry bimodal (byte per entry here)
#define NTABLES		14			// number of tagged tables

	my_update u;
	branch_info bi;
	// Circular bit history to support long histories (up to HLEN bits)
	unsigned int hpos; // next write position [0..HLEN-1]
	unsigned int hbits[HLEN/32]; // bit buffer storing taken/not-taken

	// Base predictor: 2-bit counters [0..3]
	unsigned char base[1 << BASE_BITS];

	// Tagged tables: per-bank arrays with variable sizes
	unsigned short *tags[NTABLES];
	signed char  *ctrs[NTABLES];   // 3-bit signed counters in [-4..+3]
	unsigned char *us[NTABLES];    // 2-bit usefulness counters stored in a byte (0..3)

	// Per-table parameters
	unsigned int hist_len[NTABLES];   // history lengths
	unsigned char tbl_bits[NTABLES];  // log2(entries) per table
	unsigned int tbl_size[NTABLES];   // entries per table
	unsigned char tag_bits[NTABLES];  // tag width per table (<=15)
	unsigned int salts[NTABLES];      // per-table salts for hashing

	// Simple aging counter for usefulness
	unsigned int tick;

	my_predictor (void) : hpos(0), tick(0) {
		// Initialize history buffer
		memset(hbits, 0, sizeof(hbits));
		// Configure per-table sizes, tags, and history lengths.
		// Short 6 banks: 8K entries, tags 10 bits
		// Mid 6 banks: 16K entries, tags 12–13 bits
		// Long 2 banks: 32K entries, tags 14–15 bits
		const unsigned short HL[NTABLES] = { 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 512, 800 };
		const unsigned char TB[NTABLES] = { 13,13,13,13,13,13, 14,14,14,14,14,14, 15,15 };
		const unsigned char TG[NTABLES] = { 10,10,10,10,10,10, 12,12,12,13,13,13, 14,15 };
		for (int t = 0; t < NTABLES; ++t) {
			hist_len[t] = HL[t];
			tbl_bits[t] = TB[t];
			tbl_size[t] = 1U << tbl_bits[t];
			tag_bits[t] = TG[t];
			// Simple distinct salts derived from table index
			salts[t] = 0x9E3779B9u * (t + 1) ^ (0x85EBCA6Bu + (t << 16));
		}

		memset(base, 1, sizeof(base));	// initialize T0 to weakly not taken
		// allocate and init tagged banks
		for (int t = 0; t < NTABLES; ++t) {
			tags[t] = new unsigned short[tbl_size[t]];
			ctrs[t] = new signed char[tbl_size[t]];
			us[t]   = new unsigned char[tbl_size[t]];
			memset(tags[t], 0, sizeof(unsigned short) * tbl_size[t]);
			// 3-bit signed counters start at -1 (weakly not-taken)
			for (unsigned int i = 0; i < tbl_size[t]; ++i) ctrs[t][i] = -1;
			memset(us[t],   0, sizeof(unsigned char) * tbl_size[t]);
		}
	}

	inline unsigned int mask_bits(unsigned int n) { return (1U << n) - 1U; }
	static inline unsigned int rotl32(unsigned int x, unsigned int r) { return (x << (r & 31)) | (x >> ((32 - r) & 31)); }

	inline unsigned int get_hist_bit(unsigned int back) const {
		unsigned int pos = (hpos + HLEN - 1 - (back & (HLEN - 1))) & (HLEN - 1);
		unsigned int idx = pos >> 5; // /32
		unsigned int ofs = pos & 31; // %32
		return (hbits[idx] >> ofs) & 1U;
	}

	inline unsigned int fold_history(unsigned int L) const {
		unsigned int v = 0xA5A5A5A5u;
		unsigned int step = 1; // no stride (can tweak)
		for (unsigned int i = 0; i < L; i += step) {
			v = rotl32(v, 1) ^ get_hist_bit(i);
		}
		return v;
	}

	inline unsigned int idx_calc(int t, unsigned int pc) {
		unsigned int hfold = fold_history(hist_len[t]);
		unsigned int x = pc ^ rotl32(pc, t + 1) ^ hfold ^ salts[t];
		return x & mask_bits(tbl_bits[t]);
	}

	inline unsigned short tag_calc(int t, unsigned int pc) {
		unsigned int hfold = fold_history(hist_len[t] ^ (t * 7));
		unsigned int x = (pc ^ (pc >> 7) ^ rotl32(pc, 13 + t) ^ hfold ^ (salts[t] * 0x27D4EB2Du));
		return (unsigned short)(x & mask_bits(tag_bits[t]));
	}

	// Base bimodal (2-bit) helpers
	inline bool bctr_pred(unsigned char c) { return c >= 2; }
	inline void bctr_inc(unsigned char &c) { if (c < 3) ++c; }
	inline void bctr_dec(unsigned char &c) { if (c > 0) --c; }
	inline bool bctr_weak(unsigned char c) { return c == 1 || c == 2; }

	// Tagged (3-bit signed) helpers: range [-4..+3], taken if >= 0
	inline bool tctr_pred(signed char c) { return c >= 0; }
	inline void tctr_train(signed char &c, bool taken) {
		if (taken) { if (c < 3) ++c; }
		else { if (c > -4) --c; }
	}

	branch_update *predict (branch_info & b) {
		bi = b;
		u.used_alt = false;	// altpred not used by default
		u.provider = -1;	// base table used by default
		u.alt_provider = -1;	// initialize alt_provider

		if (b.br_flags & BR_CONDITIONAL) {
			// Base predictor
			u.base_index = b.address & mask_bits(BASE_BITS);
			bool base_pred = bctr_pred(base[u.base_index]);

			// Probe tagged tables
			int provider = -1;	// base by default
			int alt_provider = -1;	// also base by default
			signed char provider_ctr = -1;
			bool alt_pred = base_pred;
			for (int t = NTABLES - 1; t >= 0; --t) {	// start from table w/ longest history
				unsigned int idx = idx_calc(t, b.address);
				u.idx[t] = idx;
				unsigned short tg = tag_calc(t, b.address);
				if (tags[t][idx] == tg) {
					if (provider < 0) {
						provider = t;
						provider_ctr = ctrs[t][idx];
					} else if (alt_provider < 0) {
						alt_provider = t;
						alt_pred = tctr_pred(ctrs[t][idx]);
					}
				}
			}

			bool final_pred = base_pred;
			if (provider >= 0) {
				bool ppred = tctr_pred(provider_ctr);
				// Use alternate if provider is weak and alternate exists and differs
				if ((provider_ctr == -1 || provider_ctr == 0) && alt_provider >= 0) {
					final_pred = alt_pred;
					u.used_alt = (final_pred != ppred);
				} else {
					final_pred = ppred;
					u.used_alt = false;
				}
				u.provider = provider;
				u.alt_provider = alt_provider; // store identified alternate provider index
			}

			u.direction_prediction(final_pred);	// update prediction with final_pred
		} else {
			u.direction_prediction(true);	// taken for non-branch instructions
		}

		u.target_prediction(0);
		return &u;
	}

	void update (branch_update *u_in, bool taken, unsigned int target) {
		(void)target; // not used ???
		if (bi.br_flags & BR_CONDITIONAL) {
			// cast back into my_update to access fields
			my_update *mu = (my_update*)u_in;
			// Always increment base predictor if taken
			if (taken) bctr_inc(base[mu->base_index]);
			// Always decrement base predictor if not taken
			else bctr_dec(base[mu->base_index]);

			// Provider/alternate update context
			int provider = mu->provider;
			int alt_provider = mu->alt_provider;
			bool provider_pred = false;
			bool alt_pred = false;
			signed char *pctr = 0;
			signed char *actr = 0;
			if (alt_provider >= 0) {
				actr = &ctrs[alt_provider][mu->idx[alt_provider]];
				alt_pred = tctr_pred(*actr);
			}
			if (provider >= 0) {
				// update provider counter
				pctr = &ctrs[provider][mu->idx[provider]];
				provider_pred = tctr_pred(*pctr);
				tctr_train(*pctr, taken);
				// If we used the alternate, also train its counter toward the outcome
				if (mu->used_alt && alt_provider >= 0 && actr) {
					tctr_train(*actr, taken);
				}
				// Update usefulness if alternate disagreed (i.e., when used)
				unsigned char &pu = us[provider][mu->idx[provider]];
				if (mu->used_alt) {
					// Reward provider if it would have been correct when overridden; otherwise penalize
					if (provider_pred == taken) { if (pu < 3) ++pu; } else { if (pu > 0) --pu; }
					// Also update alternate usefulness if available
					if (alt_provider >= 0) {
						unsigned char &au = us[alt_provider][mu->idx[alt_provider]];
						if (alt_pred == taken) { if (au < 3) ++au; } else { if (au > 0) --au; }
					}
				}
			}

			// Allocation policy: allocate only when the chosen path was not already correct.
			// Concretely: if provider exists and is wrong, allocate unless we used a correct alternate.
			// If no provider, allocate only when base was wrong.
			bool provider_wrong = (provider >= 0) ? (provider_pred != taken) : false;
			bool base_pred = bctr_pred(base[mu->base_index]);
			bool should_alloc = false;
			if (provider >= 0) {
				if (provider_wrong) {
					// If alternate was used and correct, skip allocation
					if (mu->used_alt && alt_provider >= 0) {
						should_alloc = (alt_pred != taken);
					} else {
						should_alloc = true;
					}
				}
			} else {
				should_alloc = (base_pred != taken);
			}
			if (should_alloc) {
				int allocs = 0;
				int start = (provider < 0) ? 0 : (provider + 1);
				for (int t = start; t < NTABLES && allocs < 2; ++t) {
					unsigned int idx = idx_calc(t, bi.address);
					unsigned short tg = tag_calc(t, bi.address);
					if (tags[t][idx] != tg) {
						// Prefer entries with low usefulness
						if (us[t][idx] == 0) {
							tags[t][idx] = tg;
							ctrs[t][idx] = taken ? 0 : -1; // weak toward outcome (borderline)
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
					ctrs[t][idx] = taken ? 0 : -1;
					us[t][idx] = 0;
				}
			}

			// Update circular history with conditional outcome
			unsigned int idx = hpos >> 5;
			unsigned int ofs = hpos & 31;
			unsigned int m = 1U << ofs;
			hbits[idx] = (hbits[idx] & ~m) | (taken ? m : 0U);
			hpos = (hpos + 1) & (HLEN - 1);

			// Occasionally age usefulness counters (cheap global decay)
			if ((++tick & 0x3FFFF) == 0) { // every ~262k updates
				for (int t = 0; t < NTABLES; ++t) {
					for (unsigned int i = 0; i < tbl_size[t]; ++i) {
						if (us[t][i] > 0) --us[t][i];
					}
				}
			}
		}
	}
};
