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
	fp_seconds total_time;
	double total_energy_j;
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
	return 0;
}
