/* ------------------------------------------------------------------------- */
/* Track-name intelligence                                                    */
/* ------------------------------------------------------------------------- */

static void gp_adv_normalize_name(const char *src, char *dst, size_t size)
{
	size_t i = 0, o = 0;

	if(!dst || !size)
		return;
	dst[0] = '\0';
	if(!src)
		return;

	while(src[i] && (o + 1 < size))
	{
		unsigned char ch = (unsigned char)src[i++];
		if(ch < 128)
		{
			if(isalnum(ch))
				dst[o++] = (char)tolower(ch);
			else if((o > 0) && (dst[o - 1] != ' '))
				dst[o++] = ' ';
			continue;
		}
		/* Fold the common Latin-1 UTF-8 characters used in Portuguese,
		 * Spanish, French, Italian and German track names. */
		if((ch == 0xC3) && src[i])
		{
			unsigned char lo = (unsigned char)src[i++];
			char repl = 0;
			switch(lo)
			{
				case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
				case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: repl = 'a'; break;
				case 0x87: case 0xA7: repl = 'c'; break;
				case 0x88: case 0x89: case 0x8A: case 0x8B:
				case 0xA8: case 0xA9: case 0xAA: case 0xAB: repl = 'e'; break;
				case 0x8C: case 0x8D: case 0x8E: case 0x8F:
				case 0xAC: case 0xAD: case 0xAE: case 0xAF: repl = 'i'; break;
				case 0x91: case 0xB1: repl = 'n'; break;
				case 0x92: case 0x93: case 0x94: case 0x95: case 0x96:
				case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: repl = 'o'; break;
				case 0x99: case 0x9A: case 0x9B: case 0x9C:
				case 0xB9: case 0xBA: case 0xBB: case 0xBC: repl = 'u'; break;
				case 0x9D: case 0xBD: case 0xBF: repl = 'y'; break;
				case 0x9F:
					if(o + 2 < size) { dst[o++] = 's'; dst[o++] = 's'; }
					break;
			}
			if(repl)
				dst[o++] = repl;
			continue;
		}
		if((o > 0) && (dst[o - 1] != ' '))
			dst[o++] = ' ';
	}
	while(o && (dst[o - 1] == ' '))
		o--;
	dst[o] = '\0';
}

static int gp_adv_contains_any(const char *name, const char *const *terms)
{
	unsigned i;
	if(!name || !terms)
		return 0;
	for(i = 0; terms[i]; i++)
	{
		if(strstr(name, terms[i]))
			return 1;
	}
	return 0;
}

static unsigned char gp_adv_classify_track(unsigned long track)
{
	char name[512];
	static const char *const drums[] = {
		"drum", "drums", "drummer", "drumset", "drum kit", "bateria", "baterias", "batterie",
		"batteria", "schlagzeug", "percussion", "percussao", "percusion", "percussione", "kit",
		"batucada", "tambores", "tambor", "udarn", "dobos", "perkusja", NULL
	};
	static const char *const bass[] = {
		"bass", "basso", "baixo", "bajo", "basse", "bassgitarre", "bass guitar", "contrabaixo",
		"contrabajo", "contrabass", "baixo eletrico", "electric bass", "basse electrique", "bajo electrico",
		"basgit", "gitara basowa", NULL
	};
	static const char *const guitar[] = {
		"guitar", "guitarra", "guitare", "gitarre", "gitar", "chitarra", "gitara", "violao", "violon",
		"lead", "solo", "rhythm", "rythm", "ritmo", "acoustic", "acustica", "acustico", "clean",
		"distortion", "distorted", "riff", "power chord", "rhythmus", "acompanamiento", "acompanhamento", NULL
	};
	static const char *const vocal[] = {
		"vocal", "vocals", "voice", "voz", "vozes", "voce", "voix", "canto", "cantante", "singer",
		"chant", "gesang", "melodia vocal", "melody vocal", "lead vocal", "backing vocal", "choir", "coro", NULL
	};

	if(!gp_adv_gp || (track >= gp_adv_gp->numtracks))
		return GP_ADV_KIND_OTHER;
	gp_adv_normalize_name(gp_adv_gp->names[track], name, sizeof(name));

	/* A strong name match wins.  This matters for GP files whose MIDI patch is
	 * generic/misleading (for example a vocal melody authored on a guitar staff). */
	if(gp_adv_contains_any(name, drums)) return GP_ADV_KIND_DRUM;
	if(gp_adv_contains_any(name, bass)) return GP_ADV_KIND_BASS;
	if(gp_adv_contains_any(name, vocal)) return GP_ADV_KIND_VOCAL;
	if(gp_adv_contains_any(name, guitar)) return GP_ADV_KIND_GUITAR;

	if(gp_adv_gp->instrument_types)
	{
		if(gp_adv_gp->instrument_types[track] == 3) return GP_ADV_KIND_DRUM;
		if(gp_adv_gp->instrument_types[track] == 2) return GP_ADV_KIND_BASS;
		if(gp_adv_gp->instrument_types[track] == 1) return GP_ADV_KIND_GUITAR;
	}
	return GP_ADV_KIND_OTHER;
}

static int gp_adv_name_guitar_role(unsigned long track)
{
	char name[512];
	static const char *const lead[] = {
		"lead", "solo", "solista", "principal", "melody", "melodia", "guitarra 1", "guitar 1", "guitare 1",
		"chitarra 1", "leadgitarre", "sologitarre", "guitarra lider", "guitare principale", "chitarra solista",
		"gitara prowadzaca", "first guitar", NULL
	};
	static const char *const rhythm[] = {
		"rhythm", "rythm", "ritmo", "ritmica", "rhythmus", "accompaniment", "acompanhamento", "acompanamiento",
		"accompagnement", "backing guitar", "base guitar", "guitarra base", "guitarra 2", "guitar 2", "second guitar", NULL
	};
	static const char *const bonus[] = {
		"bonus", "acoustic", "acustica", "acustico", "clean", "alternate", "alt guitar", "third guitar",
		"guitarra 3", "guitar 3", "additional guitar", "extra guitar", "nylon", NULL
	};
	gp_adv_normalize_name(gp_adv_gp->names[track], name, sizeof(name));
	if(gp_adv_contains_any(name, lead)) return 1;
	if(gp_adv_contains_any(name, rhythm)) return 2;
	if(gp_adv_contains_any(name, bonus)) return 3;
	return 0;
}

static int gp_adv_name_drum_diff(unsigned long track)
{
	char name[512];
	static const char *const bsc[] = {"bsc", "basic", "beginner", "iniciante", "facil", "easy", "debut", "novice", "einfach", NULL};
	static const char *const adv[] = {"adv", "advanced", "medium", "medio", "intermediate", "normal", "moyen", "mittel", NULL};
	static const char *const ext[] = {"ext", "extreme", "hard", "dificil", "difficile", "schwer", NULL};
	static const char *const mas[] = {"mas", "master", "maestro", "mestre", "maitre", NULL};
	static const char *const ult[] = {"ult", "ultimate", "expert", "challenge", "insane", "maximum", "maximo", NULL};
	gp_adv_normalize_name(gp_adv_gp->names[track], name, sizeof(name));
	if(gp_adv_contains_any(name, ult)) return GP_ADV_DEST_DTX_ULT;
	if(gp_adv_contains_any(name, mas)) return GP_ADV_DEST_DTX_MAS;
	if(gp_adv_contains_any(name, ext)) return GP_ADV_DEST_DTX_EXT;
	if(gp_adv_contains_any(name, adv)) return GP_ADV_DEST_DTX_ADV;
	if(gp_adv_contains_any(name, bsc)) return GP_ADV_DEST_DTX_BSC;
	return GP_ADV_DEST_NONE;
}

static int gp_adv_first_unused(const int *used, const int *choices, unsigned count)
{
	unsigned i;
	for(i = 0; i < count; i++)
	{
		if(!used[choices[i]])
			return choices[i];
	}
	return GP_ADV_DEST_NONE;
}

static unsigned long gp_adv_hash_name(const char *name, unsigned long salt)
{
	char normalized[512];
	unsigned long hash = 2166136261UL ^ salt;
	unsigned i;
	gp_adv_normalize_name(name, normalized, sizeof(normalized));
	for(i = 0; normalized[i]; i++)
	{
		hash ^= (unsigned char)normalized[i];
		hash *= 16777619UL;
	}
	return hash;
}

static int gp_adv_random_unused_drum_dest(const int *used, const int *allowed, unsigned count, unsigned long track)
{
	int choices[5];
	unsigned i, available = 0;
	unsigned long hash;
	for(i = 0; (i < count) && (i < 5); i++)
	{
		if(!used[allowed[i]])
			choices[available++] = allowed[i];
	}
	if(!available)
		return GP_ADV_DEST_NONE;
	/* Deliberately pseudo-random but repeatable for the same GP track list.  A
	 * fresh import therefore does not shuffle destinations just because the
	 * user reopened the dialog, while unnamed drum tracks are not assigned by
	 * a misleading fixed order. */
	hash = gp_adv_hash_name(gp_adv_gp->names[track], track * 2654435761UL + gp_adv_gp->numtracks);
	return choices[hash % available];
}

static void gp_adv_autofill_assignments(void)
{
	unsigned long i, drum_count = 0, drum_seen = 0;
	int used[GP_ADV_DEST_COUNT] = {0};
	static const int guitar_order[] = {GP_ADV_DEST_GUITAR, GP_ADV_DEST_GUITAR22, GP_ADV_DEST_GUITAR_BONUS};
	static const int bass_order[] = {GP_ADV_DEST_BASS, GP_ADV_DEST_BASS22};
	static const int drum_order[] = {GP_ADV_DEST_DTX_ULT, GP_ADV_DEST_DTX_BSC, GP_ADV_DEST_DTX_ADV, GP_ADV_DEST_DTX_EXT, GP_ADV_DEST_DTX_MAS};

	/* Classify first so drum cardinality can control the exact requested policy. */
	for(i = 0; i < gp_adv_gp->numtracks; i++)
	{
		gp_adv_kind[i] = gp_adv_classify_track(i);
		gp_adv_assign[i] = GP_ADV_DEST_NONE;
		gp_adv_finger[i] = 0;
		gp_adv_avoid_open[i] = 1;
		if(gp_adv_kind[i] == GP_ADV_KIND_DRUM)
			drum_count++;
	}

	/* Strings: at most three guitars and two basses receive destinations. */
	for(i = 0; i < gp_adv_gp->numtracks; i++)
	{
		int dest = GP_ADV_DEST_NONE;
		if(gp_adv_kind[i] == GP_ADV_KIND_GUITAR)
		{
			int role = gp_adv_name_guitar_role(i);
			if((role == 1) && !used[GP_ADV_DEST_GUITAR]) dest = GP_ADV_DEST_GUITAR;
			else if((role == 2) && !used[GP_ADV_DEST_GUITAR22]) dest = GP_ADV_DEST_GUITAR22;
			else if((role == 3) && !used[GP_ADV_DEST_GUITAR_BONUS]) dest = GP_ADV_DEST_GUITAR_BONUS;
			else dest = gp_adv_first_unused(used, guitar_order, 3);
		}
		else if(gp_adv_kind[i] == GP_ADV_KIND_BASS)
		{
			dest = gp_adv_first_unused(used, bass_order, 2);
		}
		if(dest)
		{
			gp_adv_assign[i] = dest;
			used[dest] = 1;
		}
	}

	/* One drum is always ULT.  Two are always ULT + BSC, exactly as specified,
	 * regardless of their names. */
	if(drum_count <= 2)
	{
		for(i = 0; i < gp_adv_gp->numtracks; i++)
		{
			if(gp_adv_kind[i] != GP_ADV_KIND_DRUM)
				continue;
			if(drum_seen == 0) gp_adv_assign[i] = GP_ADV_DEST_DTX_ULT;
			else if(drum_seen == 1) gp_adv_assign[i] = GP_ADV_DEST_DTX_BSC;
			drum_seen++;
		}
		return;
	}

	/* Three through five drum tracks: only the first five are candidates.  Give
	 * explicit BSC/ADV/EXT/MAS/ULT names first refusal, but only when that
	 * difficulty belongs to the pool available for this drum count. */
	for(i = 0; i < gp_adv_gp->numtracks && drum_seen < 5; i++)
	{
		int named, allowed = 0;
		unsigned j, pool = (drum_count > 5) ? 5U : (unsigned)drum_count;
		if(gp_adv_kind[i] != GP_ADV_KIND_DRUM)
			continue;
		named = gp_adv_name_drum_diff(i);
		for(j = 0; j < pool; j++)
		{
			if(drum_order[j] == named) { allowed = 1; break; }
		}
		if(named && allowed && !used[named])
		{
			gp_adv_assign[i] = named;
			used[named] = 1;
		}
		drum_seen++;
	}

	/* Any still-unassigned candidates get one of the remaining difficulty slots
	 * pseudo-randomly.  Sixth and later drum tracks remain blank. */
	drum_seen = 0;
	for(i = 0; i < gp_adv_gp->numtracks; i++)
	{
		unsigned pool = (drum_count > 5) ? 5U : (unsigned)drum_count;
		int dest;
		if(gp_adv_kind[i] != GP_ADV_KIND_DRUM)
			continue;
		if(drum_seen >= 5)
		{
			gp_adv_assign[i] = GP_ADV_DEST_NONE;
			drum_seen++;
			continue;
		}
		if(gp_adv_assign[i] == GP_ADV_DEST_NONE)
		{
			dest = gp_adv_random_unused_drum_dest(used, drum_order, pool, i);
			gp_adv_assign[i] = dest;
			if(dest) used[dest] = 1;
		}
		drum_seen++;
	}
}
