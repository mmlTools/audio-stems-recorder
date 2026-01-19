#include "settings.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <string>

namespace stems {

static const char *k_config_file = "audio-stems-recorder.json";

static std::string module_config_path(const char *filename)
{
	char *p = obs_module_config_path(filename);
	if (!p)
		return {};
	std::string out = p;
	bfree(p);
	return out;
}

static bool write_text_file(const std::string &path, const std::string &text)
{
	const QFileInfo fi(QString::fromStdString(path));
	QDir().mkpath(fi.absolutePath());

	QFile f(QString::fromStdString(path));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	const QByteArray bytes = QByteArray::fromStdString(text);
	f.write(bytes);
	f.close();
	return true;
}

static std::string default_output_dir()
{
	std::string p = module_config_path(".");
	return p.empty() ? std::string{} : p;
}

Settings load_settings()
{
	Settings s;
	s.output_dir = default_output_dir();

	const std::string path = module_config_path(k_config_file);
	if (path.empty())
		return s;

	QFile f(QString::fromStdString(path));
	if (!f.exists() || !f.open(QIODevice::ReadOnly))
		return s;

	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
	if (!doc.isObject())
		return s;
	const QJsonObject root = doc.object();

	// Simple fields
	s.trigger_recording = root.value("trigger_recording").toBool(true);
	s.trigger_streaming = root.value("trigger_streaming").toBool(true);
	const QString out = root.value("output_dir").toString().trimmed();
	if (!out.isEmpty())
		s.output_dir = out.toStdString();

	// Post-processing
	s.trim_silence = root.value("trim_silence").toBool(true);
	s.trim_threshold_dbfs = (float)root.value("trim_threshold_dbfs").toDouble(-45.0);
	s.trim_lead_ms = root.value("trim_lead_ms").toInt(150);
	s.trim_trail_ms = root.value("trim_trail_ms").toInt(350);

	s.normalize_audio = root.value("normalize_audio").toBool(true);
	s.normalize_target_dbfs = (float)root.value("normalize_target_dbfs").toDouble(-16.0);
	s.normalize_limiter = root.value("normalize_limiter").toBool(true);

	// Metadata
	s.write_sidecar_json = root.value("write_sidecar_json").toBool(true);
	s.record_scene_markers = root.value("record_scene_markers").toBool(true);

	// Naming
	s.use_source_aliases = root.value("use_source_aliases").toBool(false);

	// Arrays
	const QJsonArray uuids = root.value("selected_source_uuids").toArray();
	s.selected_source_uuids.clear();
	s.selected_source_uuids.reserve(uuids.size());
	for (const auto &v : uuids) {
		const QString u = v.toString().trimmed();
		if (!u.isEmpty())
			s.selected_source_uuids.emplace_back(u.toStdString());
	}

	const QJsonArray aliases = root.value("source_aliases").toArray();
	s.source_aliases.clear();
	s.source_aliases.reserve(aliases.size());
	for (const auto &v : aliases) {
		if (!v.isObject())
			continue;
		const QJsonObject o = v.toObject();
		const QString uuid = o.value("uuid").toString().trimmed();
		const QString alias = o.value("alias").toString().trimmed();
		if (!uuid.isEmpty() && !alias.isEmpty())
			s.source_aliases.emplace_back(uuid.toStdString(), alias.toStdString());
	}

	// sanitize
	s.selected_source_uuids.erase(
		std::remove_if(s.selected_source_uuids.begin(), s.selected_source_uuids.end(),
				[](const std::string &x) { return x.empty(); }),
		s.selected_source_uuids.end());

	return s;
}

void save_settings(const Settings &s)
{
	const std::string path = module_config_path(k_config_file);
	if (path.empty())
		return;

	QJsonObject root;
	root["trigger_recording"] = s.trigger_recording;
	root["trigger_streaming"] = s.trigger_streaming;
	root["output_dir"] = QString::fromStdString(s.output_dir);

	root["trim_silence"] = s.trim_silence;
	root["trim_threshold_dbfs"] = s.trim_threshold_dbfs;
	root["trim_lead_ms"] = s.trim_lead_ms;
	root["trim_trail_ms"] = s.trim_trail_ms;

	root["normalize_audio"] = s.normalize_audio;
	root["normalize_target_dbfs"] = s.normalize_target_dbfs;
	root["normalize_limiter"] = s.normalize_limiter;

	root["write_sidecar_json"] = s.write_sidecar_json;
	root["record_scene_markers"] = s.record_scene_markers;
	root["use_source_aliases"] = s.use_source_aliases;

	QJsonArray uuids;
	for (const auto &uuid : s.selected_source_uuids) {
		if (!uuid.empty())
			uuids.push_back(QString::fromStdString(uuid));
	}
	root["selected_source_uuids"] = uuids;

	QJsonArray aliases;
	for (const auto &p : s.source_aliases) {
		if (p.first.empty() || p.second.empty())
			continue;
		QJsonObject o;
		o["uuid"] = QString::fromStdString(p.first);
		o["alias"] = QString::fromStdString(p.second);
		aliases.push_back(o);
	}
	root["source_aliases"] = aliases;

	const QJsonDocument doc(root);
	write_text_file(path, doc.toJson(QJsonDocument::Compact).toStdString());
}

} // namespace stems
