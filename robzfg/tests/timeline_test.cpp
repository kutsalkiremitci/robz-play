// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play frame generation - tests of the output timeline (timeline.h).
//
// Each test plays a made-up stream through StreamClock / OutputTimeline the way the frame
// generation stage does and looks at when every picture would have been shown. No GPU and
// no video involved: frames are arrival times, generating is a duration.

#include "robzfg/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace robzfg;

namespace
{

int failures = 0;

#define CHECK(condition, ...) \
	do \
	{ \
		if(!(condition)) \
		{ \
			failures++; \
			printf("    FAILED %s:%d: %s  (", __FILE__, __LINE__, #condition); \
			printf(__VA_ARGS__); \
			printf(")\n"); \
		} \
	} while(0)

// Small deterministic generator: the tests have to give the same numbers on every run.
struct Random
{
	uint64_t state;
	explicit Random(uint64_t seed) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
	double uniform() // [0, 1)
	{
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return (double)(state >> 11) / 9007199254740992.0;
	}
	double exponential(double mean) { return -mean * std::log(1.0 - uniform()); }
};

const double NOMINAL_PERIOD = 1000000.0 / 60.0;
// what a "60 fps" stream really runs at
const double TRUE_PERIOD = 1000000.0 / 59.94;

struct Scenario
{
	std::string name;
	double seconds = 20.0;
	unsigned int target_fps = 60;
	// slots between two new pictures of the game, and how often one comes a slot later
	int game_slots = 2;
	double dip_share = 0.0;
	// lateness of arrivals: an exponential part, and now and then a larger hold-up
	double jitter_mean_us = 600.0;
	// New pictures are large and arrive later than repeats, by an amount that depends on the
	// picture. Measured on a PS5 stream at 100 Mbit/s: 1.5 ms for one in ten, 11.5 ms in the
	// middle, 14.7 ms for nine in ten.
	double picture_late_min_us = 2000.0;
	double picture_late_max_us = 15500.0;
	double spike_share = 0.0;
	double spike_min_us = 6000.0;
	double spike_max_us = 14000.0;
	// cost of generating
	double first_us = 8000.0;
	double rest_us = 2000.0;
	// stream frame at which the console leaves `skip_slots` slots out (0: never)
	int skip_at = 0;
	int skip_slots = 0;
	// stream frame at which everything is held up for `stall_us` and then arrives at once
	int stall_at = 0;
	double stall_us = 0.0;
	// stream frame lost on the way (the decoder reports it with the next one)
	int lost_at = 0;
};

struct Result
{
	int pictures = 0;      // new pictures of the game
	int generated = 0;
	int held = 0;
	int immediate = 0;
	int late = 0;          // shown more than 4 ms after they were due
	int late_generated = 0;
	double latency_avg_us = 0.0;
	double latency_min_us = 0.0;
	double latency_max_us = 0.0;
	// how far the pictures shown were from an even grid of their own content times
	double grid_error_p95_us = 0.0;
	double grid_error_max_us = 0.0;
	// the same for the pictures of the game alone: these must never move
	double real_grid_error_p95_us = 0.0;
	// time between two pictures shown one after the other
	double interval_p05_us = 0.0;
	double interval_p95_us = 0.0;
	unsigned int shifts = 0;
	int nominal_end = 0;
	double clock_error_avg_us = 0.0;
};

double percentile(std::vector<double> values, double p)
{
	if(values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	const size_t index = (size_t)std::min<double>((double)values.size() - 1.0, std::floor(p * (double)values.size()));
	return values[index];
}

Result run(const Scenario &scenario, uint64_t seed)
{
	Random random(seed);
	OutputTimeline timeline;
	timeline.reset(NOMINAL_PERIOD, scenario.target_fps, 3);

	const int frames = (int)(scenario.seconds * 59.94);
	const double base = 5000000.0; // the clocks of console and computer have nothing in common

	Result result;
	// content time (in slots of the console) and time shown of every picture put on screen
	std::vector<double> content_slot;
	std::vector<double> shown_us;
	std::vector<bool> shown_generated;
	std::vector<double> latencies;
	double clock_error_sum = 0.0;
	int clock_error_count = 0;

	double console_offset_slots = 0.0; // slots the console left out so far
	int next_new = 0;                  // stream frame of the game's next new picture
	double previous_content = 0.0;
	bool have_previous_content = false;
	double stall_until = 0.0;
	double last_shown = 0.0;
	double last_due = 0.0;
	int pending_lost = 0;
	OutputTimeline::Costs costs;
	costs.first_us = scenario.first_us;
	costs.rest_us = scenario.rest_us;
	costs.ready_delay_us = scenario.first_us + 500.0;

	for(int n = 0; n < frames; n++)
	{
		if(scenario.skip_at && n == scenario.skip_at)
			console_offset_slots += scenario.skip_slots;
		const double sent = base + ((double)n + console_offset_slots) * TRUE_PERIOD;

		const bool is_new = n == next_new;
		if(is_new)
		{
			next_new = n + scenario.game_slots;
			if(random.uniform() < scenario.dip_share)
				next_new++;
		}

		double lateness = random.exponential(scenario.jitter_mean_us);
		if(random.uniform() < scenario.spike_share)
			lateness += scenario.spike_min_us + random.uniform() * (scenario.spike_max_us - scenario.spike_min_us);
		if(is_new)
		{
			// most pictures towards the late end, as measured
			const double u = std::sqrt(random.uniform());
			lateness += scenario.picture_late_min_us + u * (scenario.picture_late_max_us - scenario.picture_late_min_us);
		}
		double arrival = sent + lateness;
		if(scenario.stall_at && n == scenario.stall_at)
			stall_until = arrival + scenario.stall_us;
		arrival = std::max(arrival, stall_until);

		if(scenario.lost_at && n == scenario.lost_at && !is_new)
		{
			pending_lost++;
			continue;
		}

		const uint64_t arrival_us = (uint64_t)arrival;
		timeline.observe(arrival_us, 1 + pending_lost);
		pending_lost = 0;
		const double content = (double)n + console_offset_slots;
		if(!is_new)
		{
			// repeats are the frames that arrive without delay: they show how well the
			// clock sits on the stream
			clock_error_sum += timeline.streamClock().lastError();
			clock_error_count++;
			continue;
		}

		// what the stage does with a new picture: recognise it, hand it to the GPU, plan
		const uint64_t now_us = arrival_us + 600;
		const OutputTimeline::Plan plan = timeline.plan(now_us, costs, true);
		result.pictures++;
		result.held += plan.held;
		if(plan.immediate)
			result.immediate++;
		else
			latencies.push_back(plan.latency_us);

		auto show = [&](double due, double ready, double content_at, bool generated) {
			due = std::max(due, last_due); // the stage never lets due times run backwards
			last_due = due;
			const double shown = std::max(std::max(due, ready), last_shown);
			last_shown = shown;
			if(shown - due > 4000.0)
			{
				result.late++;
				if(generated)
					result.late_generated++;
			}
			content_slot.push_back(content_at);
			shown_us.push_back(shown);
			shown_generated.push_back(generated);
		};

		double ready = (double)now_us;
		for(size_t k = 0; k < plan.generated.size(); k++)
		{
			ready += k == 0 ? scenario.first_us * (0.9 + 0.2 * random.uniform()) : scenario.rest_us;
			const double content_at = previous_content + (content - previous_content) * plan.generated[k].t;
			show((double)plan.generated[k].due_us, ready, have_previous_content ? content_at : content, true);
			result.generated++;
		}
		show((double)plan.real_due_us, ready, content, false);
		previous_content = content;
		have_previous_content = true;
	}

	// Leave the first two seconds out of the measurements: the clock and the delay settle.
	const double settle = base + 2000000.0;
	std::vector<double> grid_errors;
	std::vector<double> intervals;
	// The even grid: content time * true period + one constant. The constant is whatever
	// makes the errors smallest, i.e. their median.
	std::vector<double> offsets;
	std::vector<double> real_offsets;
	for(size_t i = 0; i < shown_us.size(); i++)
	{
		if(shown_us[i] <= settle)
			continue;
		offsets.push_back(shown_us[i] - content_slot[i] * TRUE_PERIOD);
		if(!shown_generated[i])
			real_offsets.push_back(offsets.back());
	}
	const double offset = percentile(offsets, 0.5);
	for(double value : offsets)
		grid_errors.push_back(std::abs(value - offset));
	std::vector<double> real_grid_errors;
	for(double value : real_offsets)
		real_grid_errors.push_back(std::abs(value - offset));
	result.real_grid_error_p95_us = percentile(real_grid_errors, 0.95);
	for(size_t i = 1; i < shown_us.size(); i++)
		if(shown_us[i] > settle)
			intervals.push_back(shown_us[i] - shown_us[i - 1]);

	result.grid_error_p95_us = percentile(grid_errors, 0.95);
	result.grid_error_max_us = percentile(grid_errors, 1.0);
	result.interval_p05_us = percentile(intervals, 0.05);
	result.interval_p95_us = percentile(intervals, 0.95);
	result.latency_min_us = percentile(latencies, 0.0);
	result.latency_max_us = percentile(latencies, 1.0);
	double latency_sum = 0.0;
	for(double value : latencies)
		latency_sum += value;
	result.latency_avg_us = latencies.empty() ? 0.0 : latency_sum / (double)latencies.size();
	result.shifts = timeline.streamClock().jumps();
	result.nominal_end = timeline.nominalSlots();
	result.clock_error_avg_us = clock_error_count ? clock_error_sum / clock_error_count : 0.0;
	return result;
}

void print(const Scenario &scenario, const Result &r)
{
	printf("  %-36s pictures %4d  generated %4d  held %3d  late %3d  delay %.1f ms (%.1f-%.1f)  off grid p95 %.2f ms (game pictures %.2f ms)  spacing %.1f-%.1f ms\n",
		scenario.name.c_str(), r.pictures, r.generated, r.held, r.late, r.latency_avg_us / 1000.0, r.latency_min_us / 1000.0,
		r.latency_max_us / 1000.0, r.grid_error_p95_us / 1000.0, r.real_grid_error_p95_us / 1000.0, r.interval_p05_us / 1000.0,
		r.interval_p95_us / 1000.0);
}

void testSteady()
{
	Scenario s;
	s.name = "steady 30 fps game";
	const Result r = run(s, 1);
	print(s, r);
	// one generated picture for every new one, none left out, next to none late
	CHECK(r.generated >= r.pictures - 12, "generated %d of %d", r.generated, r.pictures);
	CHECK(r.held <= r.pictures / 50, "held %d", r.held);
	CHECK(r.late <= r.pictures / 100, "late %d", r.late);
	// pictures sit on the grid of their slots and one period apart
	CHECK(r.grid_error_p95_us < 1500.0, "%.0f us", r.grid_error_p95_us);
	CHECK(r.interval_p05_us > TRUE_PERIOD - 2500.0 && r.interval_p95_us < TRUE_PERIOD + 2500.0, "%.0f-%.0f us", r.interval_p05_us, r.interval_p95_us);
	// The delay is one slot, plus the time to generate, plus what new pictures arrive late
	// by (some 15 ms), and no more.
	CHECK(r.latency_avg_us > TRUE_PERIOD + 20000.0 && r.latency_avg_us < TRUE_PERIOD + 30000.0, "%.0f us", r.latency_avg_us);
	CHECK(r.shifts == 0 && r.nominal_end == 2, "jumps %u nominal %d", r.shifts, r.nominal_end);
	// the clock stays on the stream although the declared period is 0.1 % off the real one
	CHECK(r.clock_error_avg_us < 2500.0, "%.0f us", r.clock_error_avg_us);
}

void testHighDetail()
{
	Scenario s;
	s.name = "generating takes 19 ms";
	s.first_us = 19000.0;
	const Result r = run(s, 11);
	print(s, r);
	// The motion detail the user chose is kept: the delay grows by what generating takes,
	// and every pair still gets its generated picture, on time.
	CHECK(r.generated >= r.pictures - 12, "generated %d of %d", r.generated, r.pictures);
	CHECK(r.held <= r.pictures / 50, "held %d", r.held);
	CHECK(r.late <= r.pictures / 50, "late %d", r.late);
	CHECK(r.latency_avg_us > TRUE_PERIOD + 32000.0 && r.latency_avg_us < TRUE_PERIOD + 42000.0, "%.0f us", r.latency_avg_us);
	CHECK(r.grid_error_p95_us < 1500.0, "%.0f us", r.grid_error_p95_us);
}

void testTooHeavy()
{
	Scenario s;
	s.name = "generating takes 31 ms";
	s.first_us = 31000.0;
	const Result r = run(s, 12);
	print(s, r);
	// More than the time between two new pictures allows. The delay stops at its limit
	// instead of growing on; a generated picture is made where the new picture came early
	// enough for it and left out elsewhere, and the real pictures stay on their slots.
	CHECK(r.held > r.pictures / 5, "held %d of %d", r.held, r.pictures);
	CHECK(r.late <= r.pictures / 20, "late %d of %d", r.late, r.pictures);
	CHECK(r.latency_max_us < TRUE_PERIOD + 0.8 * 2.0 * TRUE_PERIOD + 1.25 * TRUE_PERIOD + 1500.0, "%.0f us", r.latency_max_us);
	CHECK(r.real_grid_error_p95_us < 1500.0, "%.0f us", r.real_grid_error_p95_us);
}

void testDips()
{
	Scenario s;
	s.name = "30 fps game, one in ten a slot late";
	s.dip_share = 0.1;
	s.seconds = 40.0;
	const Result r = run(s, 2);
	print(s, r);
	// A picture that comes a slot late leaves one slot without a new picture; nothing can
	// fill it without waiting longer for everything. What must not happen is that the
	// pictures after it are late too: the timeline stays where it was.
	CHECK(r.held > r.pictures / 20 && r.held < r.pictures / 5, "held %d of %d", r.held, r.pictures);
	CHECK(r.late <= r.pictures / 50, "late %d of %d", r.late, r.pictures);
	CHECK(r.real_grid_error_p95_us < 1500.0, "%.0f us", r.real_grid_error_p95_us);
	// the delay stays that of a 30 fps game
	CHECK(r.latency_max_us < TRUE_PERIOD + 31000.0, "%.0f us", r.latency_max_us);
	CHECK(r.nominal_end == 2, "nominal %d", r.nominal_end);
}

void testSlowGame()
{
	Scenario s;
	s.name = "game mostly below 30 fps";
	s.dip_share = 0.7;
	s.seconds = 40.0;
	const Result r = run(s, 3);
	print(s, r);
	// So many pictures a slot late that this is a slower game: the delay grows by a slot and
	// every slot gets its picture again.
	CHECK(r.nominal_end == 3, "nominal %d", r.nominal_end);
	CHECK(r.latency_max_us > 2.0 * TRUE_PERIOD + 20000.0, "%.0f us", r.latency_max_us);
	CHECK(r.held < r.pictures / 20, "held %d of %d", r.held, r.pictures);
	CHECK(r.late <= r.pictures / 50, "late %d of %d", r.late, r.pictures);
	CHECK(r.grid_error_p95_us < 2000.0, "%.0f us", r.grid_error_p95_us);
}

void testSourceAtTarget()
{
	Scenario s;
	s.name = "60 fps game, 60 fps target";
	s.game_slots = 1;
	const Result r = run(s, 4);
	print(s, r);
	// nothing to generate and nothing held back
	CHECK(r.generated == 0, "generated %d", r.generated);
	CHECK(r.immediate >= r.pictures - 1, "immediate %d of %d", r.immediate, r.pictures);
}

void testTarget120()
{
	Scenario s;
	s.name = "30 fps game, 120 fps target";
	s.target_fps = 120;
	s.first_us = 7000.0;
	s.rest_us = 1500.0;
	const Result r = run(s, 5);
	print(s, r);
	// three generated pictures for every new one (but for the few that come too late), half a slot apart
	CHECK(r.generated >= (r.pictures - 12) * 3 * 95 / 100, "generated %d for %d", r.generated, r.pictures);
	CHECK(r.interval_p05_us > TRUE_PERIOD / 2.0 - 2500.0 && r.interval_p95_us < TRUE_PERIOD / 2.0 + 2500.0, "%.0f-%.0f us", r.interval_p05_us, r.interval_p95_us);
	CHECK(r.late <= r.pictures / 25, "late %d", r.late);
}

void testJitterSpikes()
{
	Scenario s;
	s.name = "arrivals held up now and then";
	s.spike_share = 0.02;
	s.seconds = 40.0;
	const Result r = run(s, 6);
	print(s, r);
	// The delay grows to cover most of them, within its limit; the pictures stay on the grid.
	CHECK(r.latency_max_us < TRUE_PERIOD + 8000.0 * 1.15 + 1500.0 + 1.25 * NOMINAL_PERIOD + 1000.0, "%.0f us", r.latency_max_us);
	CHECK(r.late <= r.pictures / 12, "late %d of %d", r.late, r.pictures);
	CHECK(r.grid_error_p95_us < 2500.0, "%.0f us", r.grid_error_p95_us);
	CHECK(r.shifts == 0, "jumps %u", r.shifts);
}

void testConsoleSkip()
{
	Scenario s;
	s.name = "console leaves two slots out";
	s.skip_at = 600;
	s.skip_slots = 2;
	const Result r = run(s, 7);
	print(s, r);
	// Found, and the pictures after it are on time again: a handful late or left out at most.
	CHECK(r.shifts >= 1 && r.shifts <= 2, "jumps %u", r.shifts);
	CHECK(r.late <= 4, "late %d", r.late);
	CHECK(r.held <= r.pictures / 50 + 6, "held %d", r.held);
	CHECK(r.grid_error_p95_us < 1500.0, "%.0f us", r.grid_error_p95_us);
}

void testStall()
{
	Scenario s;
	s.name = "frames held up 45 ms, then a burst";
	s.stall_at = 600;
	s.stall_us = 45000.0;
	const Result r = run(s, 8);
	print(s, r);
	// Frames that were only held up catch up: the clock must not take that for missing slots.
	CHECK(r.shifts == 0, "jumps %u", r.shifts);
	CHECK(r.late <= 6, "late %d", r.late);
	CHECK(r.grid_error_p95_us < 1500.0, "%.0f us", r.grid_error_p95_us);
}

void testLostFrame()
{
	Scenario s;
	s.name = "a frame lost on the way";
	s.lost_at = 601; // a repeat: the new pictures on both sides arrive
	const Result r = run(s, 9);
	print(s, r);
	// The slot is counted although nothing arrived for it: no shift, nothing late.
	CHECK(r.shifts == 0, "jumps %u", r.shifts);
	CHECK(r.late <= 1, "late %d", r.late);
	CHECK(r.held <= r.pictures / 50, "held %d", r.held);
}

void testClockBasics()
{
	printf("  clock basics\n");
	StreamClock clock;
	clock.reset(NOMINAL_PERIOD);
	CHECK(!clock.valid(), "valid before the first frame");
	uint64_t t = 1000000;
	CHECK(clock.observe(t) == 0, "first slot");
	CHECK(clock.valid(), "not valid after the first frame");
	for(int i = 1; i <= 10; i++)
		CHECK(clock.observe(t + (uint64_t)(i * NOMINAL_PERIOD)) == i, "slot %d", i);
	CHECK(std::abs(clock.timeOf(10.0) - (double)(t + (uint64_t)(10 * NOMINAL_PERIOD))) < 5.0, "time of slot 10");
	// two frames lost: the next one is three slots on
	CHECK(clock.observe(t + (uint64_t)(13 * NOMINAL_PERIOD), 3) == 13, "slot after two lost frames");
	// a long silence starts the clock again without touching the count
	const uint64_t later = t + 5000000;
	CHECK(clock.observe(later) == 14, "slot after a silence");
	CHECK(std::abs(clock.timeOf(14.0) - (double)later) < 1.0, "anchored at the frame after the silence");
	CHECK(clock.lastError() == 0.0, "error after anchoring");
}

void testPlanBasics()
{
	printf("  plan basics\n");
	OutputTimeline timeline;
	timeline.reset(NOMINAL_PERIOD, 60, 3);
	OutputTimeline::Costs costs;
	costs.first_us = 8000.0;
	costs.rest_us = 2000.0;
	costs.ready_delay_us = 8500.0;
	uint64_t t = 2000000;
	// a 30 fps game: new, repeat, new, repeat ...
	OutputTimeline::Plan plan;
	for(int n = 0; n < 60; n++)
	{
		const uint64_t arrival = t + (uint64_t)(n * NOMINAL_PERIOD);
		timeline.observe(arrival);
		if(n % 2 == 0)
			plan = timeline.plan(arrival + 500, costs, true);
	}
	CHECK(timeline.nominalSlots() == 2, "nominal %d", timeline.nominalSlots());
	CHECK(!plan.immediate && plan.spacing == 2, "immediate %d slots %d", plan.immediate ? 1 : 0, plan.spacing);
	CHECK(plan.generated.size() == 1 && std::abs(plan.generated[0].t - 0.5f) < 0.001f, "generated %d", (int)plan.generated.size());
	if(plan.generated.size() == 1)
	{
		// the generated picture is due one slot before the real one
		const double spacing = (double)plan.real_due_us - (double)plan.generated[0].due_us;
		CHECK(std::abs(spacing - NOMINAL_PERIOD) < 50.0, "spacing %.0f us", spacing);
	}
	// nothing may be generated across a cut: the picture still takes its slot
	const uint64_t arrival = t + (uint64_t)(60 * NOMINAL_PERIOD);
	timeline.observe(arrival);
	timeline.observe(arrival + (uint64_t)NOMINAL_PERIOD);
	const OutputTimeline::Plan cut = timeline.plan(arrival + (uint64_t)NOMINAL_PERIOD + 500, costs, false);
	CHECK(cut.generated.empty() && !cut.immediate && cut.held == 0, "generated %d", (int)cut.generated.size());
	CHECK(cut.real_due_us > arrival + (uint64_t)NOMINAL_PERIOD, "real due");
	// after frames went through untouched there is nothing to count from
	timeline.forgetPrevious();
	timeline.observe(arrival + (uint64_t)(3 * NOMINAL_PERIOD), 2);
	const OutputTimeline::Plan fresh = timeline.plan(arrival + (uint64_t)(3 * NOMINAL_PERIOD) + 500, costs, true);
	CHECK(fresh.spacing == 0 && fresh.generated.empty(), "slots %d generated %d", fresh.spacing, (int)fresh.generated.size());
}

// Plays the frames of a real session (a framegen-trace-*.csv written by Robz Play) through the
// timeline: the arrival times and what each frame was are taken from the file, generating is
// a duration.
//   robzfg-timeline-test --replay <file.csv> [first ms] [target fps] [strict]
// `strict`: only pictures that are all but identical go without a generated frame, as before
// faint changes off the game's beat were left out too.
int replay(const char *file_name, double first_ms, unsigned int target_fps, bool strict)
{
	std::ifstream file(file_name);
	if(!file)
	{
		printf("could not open %s\n", file_name);
		return 1;
	}
	OutputTimeline timeline;
	timeline.reset(NOMINAL_PERIOD, target_fps, 3);
	OutputTimeline::Costs costs;
	costs.first_us = first_ms * 1000.0;
	costs.rest_us = first_ms * 250.0;
	costs.ready_delay_us = costs.first_us + 500.0;

	Random random(1);
	std::string line;
	std::getline(file, line); // column names
	int frames = 0, pictures = 0, wanted = 0, generated = 0, held = 0, late = 0, immediate = 0, still = 0;
	// generating after the GPU had nothing to do for a while is what goes wrong on a real
	// machine: count how often that would happen
	int after_pause = 0;
	double last_generated_at = 0.0;
	bool have_picture_slot = false;
	int64_t picture_slot = 0;
	std::vector<double> shown;
	std::vector<double> latencies;
	std::vector<double> lateness_seen;
	double last_shown = 0.0, last_due = 0.0;
	const double base = 1000000.0;
	while(std::getline(file, line))
	{
		// t_ms,slot,advance,kind,clock_error_ms,<late>,mean_diff,changed_ratio,...
		std::vector<std::string> cells;
		std::stringstream stream(line);
		std::string cell;
		while(std::getline(stream, cell, ','))
			cells.push_back(cell);
		if(cells.size() < 8)
			continue;
		const uint64_t arrival_us = (uint64_t)(base + atof(cells[0].c_str()) * 1000.0);
		const int advance = atoi(cells[2].c_str());
		const char kind = cells[3].empty() ? '?' : cells[3][0];
		frames++;
		const int64_t slot = timeline.observe(arrival_us, advance);
		if(kind == 'O' || kind == 'X')
		{
			timeline.forgetPrevious();
			continue;
		}
		if(kind != 'U')
			continue;
		pictures++;
		lateness_seen.push_back(timeline.streamClock().lastError());
		const double mean_diff = atof(cells[6].c_str());
		const double changed = atof(cells[7].c_str());
		const int64_t spacing = have_picture_slot ? slot - picture_slot : 0;
		picture_slot = slot;
		have_picture_slot = true;
		const bool faint = mean_diff < 0.15 && changed < 0.003;
		const bool off_beat = spacing != timeline.nominalSlots();
		const bool nearly_still = (mean_diff < 0.3 && changed < 0.0005) || (!strict && faint && off_beat);
		const bool cut = mean_diff > 35.0 && changed > 0.75;
		if(nearly_still)
			still++;
		const uint64_t now_us = arrival_us + 600;
		const OutputTimeline::Plan plan = timeline.plan(now_us, costs, !nearly_still && !cut);
		held += plan.held;
		wanted += plan.held + (int)plan.generated.size();
		if(plan.immediate)
			immediate++;
		else
			latencies.push_back(plan.latency_us);
		auto show = [&](double due, double ready) {
			due = std::max(due, last_due);
			last_due = due;
			const double at = std::max(std::max(due, ready), last_shown);
			last_shown = at;
			if(at - due > 4000.0)
				late++;
			shown.push_back(at);
		};
		double ready = (double)now_us;
		if(!plan.generated.empty())
		{
			if(last_generated_at > 0.0 && (double)now_us - last_generated_at > 250000.0)
				after_pause++;
			last_generated_at = (double)now_us;
		}
		for(size_t k = 0; k < plan.generated.size(); k++)
		{
			ready += k == 0 ? costs.first_us * (0.9 + 0.2 * random.uniform()) : costs.rest_us;
			show((double)plan.generated[k].due_us, ready);
			generated++;
		}
		show((double)plan.real_due_us, ready);
	}
	std::vector<double> intervals;
	for(size_t i = 1; i < shown.size(); i++)
		intervals.push_back(shown[i] - shown[i - 1]);
	int even = 0;
	for(double interval : intervals)
		if(std::abs(interval - TRUE_PERIOD * 60.0 / target_fps) < 2500.0)
			even++;
	printf("replay of %s: generating takes %.1f ms, target %u fps\n", file_name, first_ms, target_fps);
	printf("  frames %d, new pictures %d (%d of them all but still, %d shown at once)\n", frames, pictures, still, immediate);
	printf("  new pictures arrived late by: middle %.1f ms, nine in ten within %.1f ms, 99 in 100 within %.1f ms\n",
		percentile(lateness_seen, 0.5) / 1000.0, percentile(lateness_seen, 0.9) / 1000.0, percentile(lateness_seen, 0.99) / 1000.0);
	printf("  generated pictures wanted %d: made %d, left out %d (%.1f %%), shown late %d\n", wanted, generated, held,
		wanted ? 100.0 * held / wanted : 0.0, late);
	printf("  generating started %d times after a pause of a quarter second and more\n", after_pause);
	printf("  delay: middle %.1f ms, between %.1f and %.1f ms; the clock was set anew %u times\n", percentile(latencies, 0.5) / 1000.0,
		percentile(latencies, 0.0) / 1000.0, percentile(latencies, 1.0) / 1000.0, timeline.streamClock().jumps());
	printf("  time between pictures shown: 5 %% under %.1f ms, middle %.1f ms, 5 %% over %.1f ms; %.1f %% within 2.5 ms of one frame\n",
		percentile(intervals, 0.05) / 1000.0, percentile(intervals, 0.5) / 1000.0, percentile(intervals, 0.95) / 1000.0,
		intervals.empty() ? 0.0 : 100.0 * even / (double)intervals.size());
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	if(argc >= 3 && !strcmp(argv[1], "--replay"))
		return replay(argv[2], argc >= 4 ? atof(argv[3]) : 6.0, argc >= 5 ? (unsigned int)atoi(argv[4]) : 60u,
			argc >= 6 && !strcmp(argv[5], "strict"));

	const std::vector<std::pair<const char *, std::function<void()>>> tests = {
		{"clock", testClockBasics},
		{"plan", testPlanBasics},
		{"steady", testSteady},
		{"high detail", testHighDetail},
		{"too heavy", testTooHeavy},
		{"dips", testDips},
		{"slow game", testSlowGame},
		{"source at target", testSourceAtTarget},
		{"target 120", testTarget120},
		{"jitter spikes", testJitterSpikes},
		{"console skip", testConsoleSkip},
		{"stall", testStall},
		{"lost frame", testLostFrame},
	};
	for(const auto &test : tests)
	{
		const int before = failures;
		test.second();
		printf("%s %s\n", failures == before ? "[ OK   ]" : "[ FAIL ]", test.first);
	}
	printf("%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}
