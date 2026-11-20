#include "settings.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <string>

namespace stems {

static const char *k_config_file = "audio-stems-recorder.json";

static QString module_config_path(const char *filename)
{
	char *p = obs_module_config_path(filename);
	if (!p)
		return {};
	QString out = QString::fromUtf8(p);
	bfree(p);
	return out;
}

std::string default_output_dir()
{
	QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (base.isEmpty())
		base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
	if (base.isEmpty())
		return module_config_path(".").toStdString();
	return QDir::toNativeSeparators(QDir(base).filePath(QStringLiteral("OBS Audio Stems"))).toUtf8().toStdString();
}

static int clamp_int(int v, int lo, int hi)
{
	return std::max(lo, std::min(hi, v));
}

Settings load_settings()
{
	Settings s;
	s.output_dir = default_output_dir();

	const QString path = module_config_path(k_config_file);
	if (path.isEmpty())
		return s;

	QFile f(path);
	if (!f.exists() || !f.open(QIODevice::ReadOnly))
		return s;

	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
	if (!doc.isObject())
		return s;
	const QJsonObject root = doc.object();

	s.trigger_recording = root.value("trigger_recording").toBool(s.trigger_recording);
	s.trigger_streaming = root.value("trigger_streaming").toBool(s.trigger_streaming);
	s.save_next_to_recordings = root.value("save_next_to_recordings").toBool(s.save_next_to_recordings);
	const QString out = root.value("output_dir").toString().trimmed();
	if (!out.isEmpty())
		s.output_dir = out.toUtf8().toStdString();

	const QString fmt = root.value("output_format").toString("wav").trimmed().toLower();
	s.output_format = (fmt == "mp3" || fmt == "flac") ? fmt.toStdString() : "wav";
	const int bits = root.value("wav_bit_depth").toInt(s.wav_bit_depth);
	s.wav_bit_depth = (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
	s.mp3_bitrate_kbps = clamp_int(root.value("mp3_bitrate_kbps").toInt(s.mp3_bitrate_kbps), 64, 320);
	s.keep_wav_after_export = root.value("keep_wav_after_export").toBool(s.keep_wav_after_export);
	s.ffmpeg_path = root.value("ffmpeg_path").toString().trimmed().toUtf8().toStdString();

	s.post_fader = root.value("post_fader").toBool(s.post_fader);
	s.track_mask = (uint32_t)root.value("track_mask").toInt(0) & 0x3Fu;

	s.trim_silence = root.value("trim_silence").toBool(s.trim_silence);
	s.trim_threshold_dbfs = (float)root.value("trim_threshold_dbfs").toDouble(s.trim_threshold_dbfs);
	s.trim_lead_ms = clamp_int(root.value("trim_lead_ms").toInt(s.trim_lead_ms), 0, 10000);
	s.trim_trail_ms = clamp_int(root.value("trim_trail_ms").toInt(s.trim_trail_ms), 0, 10000);
	s.normalize_audio = root.value("normalize_audio").toBool(s.normalize_audio);
	s.normalize_target_dbfs = (float)root.value("normalize_target_dbfs").toDouble(s.normalize_target_dbfs);
	s.normalize_limiter = root.value("normalize_limiter").toBool(s.normalize_limiter);
	s.write_sidecar_json = root.value("write_sidecar_json").toBool(s.write_sidecar_json);
	s.record_scene_markers = root.value("record_scene_markers").toBool(s.record_scene_markers);
	s.use_source_aliases = root.value("use_source_aliases").toBool(s.use_source_aliases);

	// Current format: [{uuid, name}]. Older versions stored only UUIDs.
	const QJsonArray sel = root.value("selected_sources").toArray();
	for (const auto v : sel) {
		const QJsonObject o = v.toObject();
		SelectedSource ss;
		ss.uuid = o.value("uuid").toString().trimmed().toStdString();
		ss.name = o.value("name").toString().toUtf8().toStdString();
		if (!ss.uuid.empty() || !ss.name.empty())
			s.selected_sources.push_back(std::move(ss));
	}
	if (s.selected_sources.empty()) {
		const QJsonArray uuids = root.value("selected_source_uuids").toArray();
		for (const auto v : uuids) {
			const QString u = v.toString().trimmed();
			if (!u.isEmpty())
				s.selected_sources.push_back(SelectedSource{u.toStdString(), {}});
		}
	}

	const QJsonArray aliases = root.value("source_aliases").toArray();
	for (const auto v : aliases) {
		if (!v.isObject())
			continue;
		const QJsonObject o = v.toObject();
		const QString uuid = o.value("uuid").toString().trimmed();
		const QString alias = o.value("alias").toString().trimmed();
		if (!uuid.isEmpty() && !alias.isEmpty())
			s.source_aliases.emplace_back(uuid.toStdString(), alias.toUtf8().toStdString());
	}

	return s;
}

void save_settings(const Settings &s)
{
	const QString path = module_config_path(k_config_file);
	if (path.isEmpty())
		return;

	QJsonObject root;
	root["version"] = 2;
	root["trigger_recording"] = s.trigger_recording;
	root["trigger_streaming"] = s.trigger_streaming;
	root["save_next_to_recordings"] = s.save_next_to_recordings;
	root["output_dir"] = QString::fromUtf8(s.output_dir.c_str());
	root["output_format"] = QString::fromStdString(s.output_format);
	root["wav_bit_depth"] = s.wav_bit_depth;
	root["mp3_bitrate_kbps"] = s.mp3_bitrate_kbps;
	root["keep_wav_after_export"] = s.keep_wav_after_export;
	root["ffmpeg_path"] = QString::fromUtf8(s.ffmpeg_path.c_str());
	root["post_fader"] = s.post_fader;
	root["track_mask"] = (int)s.track_mask;

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

	QJsonArray sel, uuids;
	for (const auto &ss : s.selected_sources) {
		QJsonObject o;
		o["uuid"] = QString::fromStdString(ss.uuid);
		o["name"] = QString::fromUtf8(ss.name.c_str());
		sel.push_back(o);
		if (!ss.uuid.empty())
			uuids.push_back(QString::fromStdString(ss.uuid));
	}
	root["selected_sources"] = sel;
	root["selected_source_uuids"] = uuids; // keeps older plugin versions working after a downgrade

	QJsonArray aliases;
	for (const auto &p : s.source_aliases) {
		if (p.first.empty() || p.second.empty())
			continue;
		QJsonObject o;
		o["uuid"] = QString::fromStdString(p.first);
		o["alias"] = QString::fromUtf8(p.second.c_str());
		aliases.push_back(o);
	}
	root["source_aliases"] = aliases;

	QDir().mkpath(QFileInfo(path).absolutePath());
	// Atomic write: a crash while saving never leaves a truncated config.
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return;
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!f.commit())
		blog(LOG_WARNING, "[Audio Stems] could not save settings to %s", path.toUtf8().constData());
}

} // namespace stems
