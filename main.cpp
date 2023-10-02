#include <iostream>
#include <filesystem>
#include <chrono>

#include <fmt/format.h>
#include <fmt/std.h>
#include <fmt/chrono.h>
#include <argparse/argparse.hpp>
#include <date/date.h>
#include <simdjson.h>

using std::filesystem::path;
using namespace std::string_literals;
using namespace std::string_view_literals;

using simdjson::padded_string;
namespace sj = simdjson::ondemand;

using ts_time = std::chrono::sys_time<std::chrono::nanoseconds>;
using fp_seconds = std::chrono::duration<double>;

struct Measurement
{
	ts_time stamp;
	double uptime_cur, uptime_tot;
	double pwr;
};

struct Result
{
	static const constexpr double COST_KWH = 7.79;

	fp_seconds total_time;
	double total_energy_j;
	unsigned rollovers;
	bool bad;

	double total_energy_kwh() const { return total_energy_j / 3600 / 1000; }
};

double parse_item(sj::object obj, std::string_view unit)
{
	if (obj["unit"].get_string() != unit) {
		throw std::runtime_error(
			fmt::format(
				"Bad item: {}, expected unit: \"{}\"",
				obj.raw_json().value(),
				unit
			));
	}
	return obj["value"].get_double();
}

ts_time parse_timestamp(std::string_view s)
{
	// TODO: write a custom stringstream
	std::istringstream ss{std::string{s}};
	ss.exceptions(std::ios::failbit);

	ts_time ret;
	// 2023-05-31T00:13:57,906371842+03:00
	ss >> date::parse("%FT%T%Ez", ret);

	return ret;
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
			r.total_time += uptime;
			r.total_energy_j += last.pwr * uptime.count();
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

	r.total_time += delta_wall;
	r.total_energy_j += (prev.pwr + last.pwr) * delta_wall.count() / 2;
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

	for (auto doc: input_json) try {
		auto ts = parse_timestamp(doc["timestamp"].get_string());

		sj::array device_items;
		for (auto device: doc["data"].get_array()) {
			if (device["description"].get_string() == "Corsair HX1000i"sv) {
				device_items = device["status"].get_array();
				break;
			}
		}

		double uptime_cur, uptime_tot, pwr_output, pwr_input;
		for (auto i: device_items) {
			sj::object item = i.get_object();

			std::string_view key = item["key"].get_string();
			if (key == "Current uptime") {
				uptime_cur = parse_item(item, "s");
			} else if (key == "Total uptime") {
				uptime_tot = parse_item(item, "s");
			} else if (key == "Total power output") {
				pwr_output = parse_item(item, "W");
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
		fmt::print(stderr, "Failed to parse ({}):\n{}\n", e.what(), to_json_string(doc).value());
	}

	/* TODO: fmtlib does not yet support %j for durations
	 *       (https://github.com/fmtlib/fmt/issues/3643) */
	fmt::print(
		"Total time is {}d {:%Hh %Mm %Ss}\n",
		std::chrono::floor<std::chrono::days>(r.total_time).count(),
		r.total_time
	);
	fmt::print("Total energy is {} J\n", r.total_energy_j);
	fmt::print("         ... or {} kWh\n", r.total_energy_kwh());
	fmt::print("         ... or {} ₽\n", r.total_energy_kwh() * r.COST_KWH);
	fmt::print("Total rollover events: {}\n", r.rollovers);
	return r.bad ? 1 : 0;
}
