void ProduceChecksum128(void* buf, int len){
	unsigned char* bmem = (unsigned char*)buf;
	unsigned long* out = (unsigned long*)(bmem + len - 16);
	unsigned long* blocks = (unsigned long*)bmem;
	const int nblocks = (len - 13) / 4;
	const unsigned long c1 = 0x239b961b;
	const unsigned long c2 = 0xab0e9789;
	const unsigned long c3 = 0x38b34ae5;
	const unsigned long c4 = 0xa1e38b93;
	unsigned long h1 = 0;
	unsigned long h2 = 0;
	unsigned long h3 = 0;
	unsigned long h4 = 0;
	int i, j;
	out[0] = 0;
	out[1] = 0;
	out[2] = 0;
	out[3] = 0;
	for(i = 0; i < nblocks; i += 4) {
		unsigned long k1 = blocks[i + 0];
		unsigned long k2 = blocks[i + 1];
		unsigned long k3 = blocks[i + 2];
		unsigned long k4 = blocks[i + 3];
		k1 *= c1;
		k1 = _rotl(k1, 15);
		k1 *= c2;
		h1 ^= k1;
		h1 = _rotl(h1, 19);
		h1 += h2;
		h1 = h1 * 5 + 0x561ccd1b;
		k2 *= c2;
		k2 = _rotl(k2, 16);
		k2 *= c3;
		h2 ^= k2;
		h2 = _rotl(h2, 17);
		h2 += h3;
		h2 = h2 * 5 + 0x0bcaa747;
		k3 *= c3;
		k3 = _rotl(k3, 17);
		k3 *= c4;
		h3 ^= k3;
		h3 = _rotl(h3, 15);
		h3 += h4;
		h3 = h3 * 5 + 0x96cd1c35;
		k4 *= c4;
		k4 = _rotl(k4, 18);
		k4 *= c1;
		h4 ^= k4;
		h4 = _rotl(h4, 13);
		h4 += h1;
		h4 = h4 * 5 + 0x32ac3b17;
	}
	out[0] = h1;
	out[1] = h2;
	out[2] = h3;
	out[3] = h4;
}

int VerifyChecksum128(void* buf, int len){
	unsigned char* bmem = (unsigned char*)buf;
	unsigned char checksum[16];
	memcpy(checksum, bmem + len - 16, 16);
	ProduceChecksum128(buf, len);
	if(0 == memcmp(checksum, bmem + len - 16, 16)){
		return 1;
	}
	memcpy(bmem + len - 16, checksum, 16);
	return 0;
}
