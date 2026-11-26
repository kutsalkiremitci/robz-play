// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>

#include <chiaki/packetstats.h>

// Pushes `count` consecutive sequence numbers starting at `first`, leaving out every
// `skip_every`-th one (0 leaves out none). Returns the number after the last one.
static ChiakiSeqNum16 push_run(ChiakiPacketStats *stats, ChiakiSeqNum16 first, unsigned int count, unsigned int skip_every)
{
	ChiakiSeqNum16 seq = first;
	for(unsigned int i = 0; i < count; i++)
	{
		if(!skip_every || (i + 1) % skip_every != 0)
			chiaki_packet_stats_push_seq(stats, seq);
		seq++;
	}
	return seq;
}

static MunitResult test_packet_stats_seq(const MunitParameter params[], void *user)
{
	ChiakiPacketStats stats;
	munit_assert_int(chiaki_packet_stats_init(&stats), ==, CHIAKI_ERR_SUCCESS);

	uint64_t received;
	uint64_t lost;

	// nothing lost
	push_run(&stats, 1, 20, 0);
	chiaki_packet_stats_get(&stats, true, &received, &lost);
	munit_assert_uint64(received, ==, 20);
	munit_assert_uint64(lost, ==, 0);

	// 21..39 with every fourth one missing: 24, 28, 32 and 36
	push_run(&stats, 21, 19, 4);
	chiaki_packet_stats_get(&stats, true, &received, &lost);
	munit_assert_uint64(received, ==, 15);
	munit_assert_uint64(lost, ==, 4);

	chiaki_packet_stats_fini(&stats);
	return MUNIT_OK;
}

// The sequence number of the audio stream wraps every 65536 frames, about every 11 minutes.
// The interval in which that happens used to be reported as a loss of nearly everything.
static MunitResult test_packet_stats_seq_wrap(const MunitParameter params[], void *user)
{
	ChiakiPacketStats stats;
	munit_assert_int(chiaki_packet_stats_init(&stats), ==, CHIAKI_ERR_SUCCESS);

	uint64_t received;
	uint64_t lost;

	// bring the counter up to just below the wrap, in steps a real stream would take
	ChiakiSeqNum16 seq = 1;
	while(seq < 0xfff0)
	{
		seq = push_run(&stats, seq, 20, 0);
		chiaki_packet_stats_get(&stats, true, &received, &lost);
		munit_assert_uint64(received, ==, 20);
		munit_assert_uint64(lost, ==, 0);
	}

	// the interval that crosses 0xffff -> 0, nothing lost
	seq = push_run(&stats, seq, 40, 0);
	munit_assert(seq < 0x100);
	chiaki_packet_stats_get(&stats, true, &received, &lost);
	munit_assert_uint64(received, ==, 40);
	munit_assert_uint64(lost, ==, 0);

	chiaki_packet_stats_fini(&stats);
	return MUNIT_OK;
}

// A real loss in the interval that wraps is still counted, and counted correctly.
static MunitResult test_packet_stats_seq_wrap_loss(const MunitParameter params[], void *user)
{
	ChiakiPacketStats stats;
	munit_assert_int(chiaki_packet_stats_init(&stats), ==, CHIAKI_ERR_SUCCESS);

	uint64_t received;
	uint64_t lost;

	// move the counter to just below the wrap; a sequence number only counts as newer when
	// it is less than half the range ahead, hence the steps
	chiaki_packet_stats_push_seq(&stats, 0x7000);
	chiaki_packet_stats_push_seq(&stats, 0xe000);
	chiaki_packet_stats_push_seq(&stats, 0xffef);
	chiaki_packet_stats_get(&stats, true, &received, &lost);

	// 0xfff0 .. 0x0016 (39 numbers) with every fourth one missing: 9 lost, 30 received
	ChiakiSeqNum16 seq = push_run(&stats, 0xfff0, 39, 4);
	munit_assert_uint16(seq, ==, 0x0017);
	chiaki_packet_stats_get(&stats, true, &received, &lost);
	munit_assert_uint64(received, ==, 30);
	munit_assert_uint64(lost, ==, 9);

	chiaki_packet_stats_fini(&stats);
	return MUNIT_OK;
}

MunitTest tests_packet_stats[] = {
	{
		"/seq",
		test_packet_stats_seq,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/seq_wrap",
		test_packet_stats_seq_wrap,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/seq_wrap_loss",
		test_packet_stats_seq_wrap_loss,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
