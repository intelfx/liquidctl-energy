#include <iostream>
#include <filesystem>
#include <chrono>
#include <map>

#include <fmt/format.h>
#include <fmt/std.h>
#include <fmt/chrono.h>
#include <argparse/argparse.hpp>
#include <date/date.h>
#include <simdjson.h>

#include "svstream.hpp"

using std::filesystem::path;
using namespace std::string_literals;
using namespace std::string_view_literals;

using simdjson::padded_string;
namespace sj = simdjson::ondemand;

using ts_time = std::chrono::sys_time<std::chrono::nanoseconds>;
using ts_zoned = std::chrono::zoned_time<std::chrono::nanoseconds>;
using fp_seconds = std::chrono::duration<double>;

struct Measurement
{
	ts_time stamp;
	double uptime_cur, uptime_tot;
	double pwr;
};

struct GroupKey : public std::tuple<int, int>
{
public:
	GroupKey(auto &&... ts)
		: std::tuple<int, int>(std::forward<decltype(ts)>(ts)...)
	{ }

	static GroupKey from_time(ts_time ts);
};

struct GroupResult
{
	static const constexpr double COST_KWH = 7.79;
	fp_seconds time;
	double energy_j;
	double energy_kwh() const { return energy_j / 3600 / 1000; }
};

struct Result
{
	GroupResult total;
	std::map<GroupKey, GroupResult> buckets;
	unsigned rollovers;
	bool bad;
};

double parse_item(sj::object obj, std::string_view unit)
{
	auto value = obj.find_field("value").get_double().value();
	if (obj.find_field("unit").get_string() != unit) {
		throw std::runtime_error(
			fmt::format(
				"Bad item: {}, expected unit: \"{}\"",
				obj.raw_json().value(),
				unit
			));
	}
	return value;
}

ts_time parse_timestamp(std::string_view s)
{
	isvstream ss{s};
	ss.exceptions(std::ios::failbit);

	ts_time ret;
	// 2023-05-31T00:13:57,906371842+03:00
	ss >> date::parse("%FT%T%Ez", ret);

	return ret;
}

GroupKey GroupKey::from_time(ts_time ts)
{
	static auto zone = std::chrono::current_zone();

	auto ts_local = ts_zoned{zone, ts}.get_local_time();
	auto ymd = std::chrono::year_month_day{ std::chrono::floor<std::chrono::days>(ts_local) };
	return {(int)ymd.year(), (unsigned)ymd.month()};
}

void account_step(Result &r, ts_time ts, fp_seconds time, double energy)
{
	static GroupKey last_key{};
	static GroupResult *last_bucket{};

	auto key = GroupKey::from_time(ts);
	if (key != last_key) {
		last_key = key;
		last_bucket = &r.buckets[key];
	}

	r.total.time += time;
	r.total.energy_j += energy;
	last_bucket->time += time;
	last_bucket->energy_j += energy;
}

void process_step(Result &r, const Measurement &prev, const Measurement &last)
{
	fp_seconds delta_wall{last.stamp - prev.stamp};
	fp_seconds delta_uptime_tot{last.uptime_tot - prev.uptime_tot};
	fp_seconds delta_uptime_cur{last.uptime_cur - prev.uptime_cur};
	fp_seconds uptime{last.uptime_cur};

	bool delta_uptime_bad = (
		delta_uptime_tot.count() < uptime.count()
	);

	if (std::abs(delta_wall.count() - delta_uptime_tot.count()) < 2) {
		/* OK */
	} else if (std::abs(delta_uptime_tot.count() - delta_uptime_cur.count()) < 1) {
		/* imprecise wall time recorded, but no rollover has occurred -- OK for now */
	} else if (delta_wall.count() > uptime.count()) {
		fmt::print(""
			   "Rollover: at   {} uptime_cur={} uptime_tot={}\n"
			   "          prev {} uptime_cur={} uptime_tot={}\n"
			   "          wall clock delta: {}\n"
			   "              uptime delta: t. {}{}\n"
			   "                    uptime: {}\n"
			,
			   last.stamp, last.uptime_cur, last.uptime_tot,
			   prev.stamp, prev.uptime_cur, prev.uptime_tot,
			   delta_wall,
			   delta_uptime_bad ? "(invalid) " : "", delta_uptime_tot,
			   uptime
		);

		++r.rollovers;
		if (delta_uptime_bad) {
			/* total uptime was not properly updated -- assuming a power loss has occurred, use only this measurement */
			account_step(r, last.stamp, uptime, last.pwr * uptime.count());
			return;
		} else {
			/* total uptime was updated -- use that delta instead of the wall clock delta */
			delta_wall = delta_uptime_tot;
		}
	} else {
		fmt::print(""
			   "!!! INCONSISTENT MEASUREMENT !!!"
			   "          at   {} uptime_cur={} uptime_tot={}\n"
			   "          prev {} uptime_cur={} uptime_tot={}\n"
			   "          wall clock delta: {}\n"
			   "              uptime delta: t. {}{}, cur. {}\n"
			   "                    uptime: {}\n"
			,
			   last.stamp, last.uptime_cur, last.uptime_tot,
			   prev.stamp, prev.uptime_cur, prev.uptime_tot,
			   delta_wall,
			   delta_uptime_bad ? "(invalid) " : "", delta_uptime_tot, delta_uptime_cur,
			   uptime
		);

		r.bad = true;
		return;
	}

	account_step(r, prev.stamp, delta_wall, (prev.pwr + last.pwr) * delta_wall.count() / 2);
}

int main(int argc, char **argv)
{
	std::locale::global(std::locale(""));

	argparse::ArgumentParser args("liquidctl-energy");
	args.add_argument("input")
		.action([](const std::string &value) {
			return path(value);
		});

	try {
		args.parse_args(argc, argv);
	} catch (const std::runtime_error &err) {
		std::cerr << err.what() << std::endl;
		std::cerr << args << std::endl;
		std::exit(1);
	}

	auto input_path = args.get<path>("input");
	if (!exists(input_path)) {
		throw std::runtime_error(
			fmt::format(
				"Input file {} does not exist",
				input_path
			));
	}

	padded_string input_str = padded_string::load(input_path.native());

	sj::parser parser;
	sj::document_stream input_json = parser.iterate_many(input_str);

	bool is_first = true;
	Measurement prev, m;
	Result r{};

	for (sj::document_reference doc: input_json) try {
		auto ts = parse_timestamp(doc.find_field("timestamp").get_string());

		sj::array device_items;
		for (sj::object device: doc.find_field("data").get_array()) {
			if (device.find_field("description").get_string() == "Corsair HX1000i"sv) {
				device_items = device.find_field("status").get_array();
				break;
			}
		}

		double uptime_cur, uptime_tot, pwr_input;
		for (sj::object item: device_items) {
			std::string_view key = item.find_field("key").get_string();
			if (key == "Current uptime") {
				uptime_cur = parse_item(item, "s");
			} else if (key == "Total uptime") {
				uptime_tot = parse_item(item, "s");
			} else if (key == "Estimated input power") {
				pwr_input = parse_item(item, "W");
			}
		}

		m = {
			.stamp = ts,
			.uptime_cur = uptime_cur,
			.uptime_tot = uptime_tot,
			.pwr = pwr_input,
		};

		if (is_first) {
			is_first = false;
		} else {
			process_step(r, prev, m);
		}

		prev = m;
	} catch (const simdjson::simdjson_error &e) {
		fmt::print(stderr, "Failed to parse ({}):\n{}\n", e.what(), doc.raw_json().value());
	}

	fmt::print("Total rollover events: {}\n\n", r.rollovers);
	fmt::print("-----------------------------------\n");

	for (const auto &i: r.buckets) {
		/* TODO: fmtlib does not yet support %j for durations
		 *       (https://github.com/fmtlib/fmt/issues/3643) */
		fmt::print(
			"{:04d}-{:02d} uptime is {:2}d {:.1%Hh %Mm %Ss}\n",
			std::get<0>(i.first),
			std::get<1>(i.first),
			std::chrono::floor<std::chrono::days>(i.second.time).count(),
			i.second.time
		);
		fmt::print("        energy is {:>6.2f} kWh\n", i.second.energy_kwh());
		fmt::print("           ... or {:>6.2f} ₽\n", i.second.energy_kwh() * i.second.COST_KWH);
	}

	fmt::print("----------------------------------\n");

	/* TODO: fmtlib does not yet support %j for durations
	 *       (https://github.com/fmtlib/fmt/issues/3643) */
	fmt::print(
		"Total uptime is {:3}d {:.1%Hh %Mm %Ss}\n",
		std::chrono::floor<std::chrono::days>(r.total.time).count(),
		r.total.time
	);
	fmt::print("Total energy is {:>8.2f} kWh\n", r.total.energy_kwh());
	fmt::print("         ... or {:>8.2f} ₽\n", r.total.energy_kwh() * r.total.COST_KWH);

	return r.bad ? 1 : 0;
}
