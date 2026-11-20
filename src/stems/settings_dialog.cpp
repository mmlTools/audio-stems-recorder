#include "settings_dialog.hpp"

#include "transcode.hpp"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>

#include <obs-module.h>

#include <set>
#include <string>
#include <vector>

namespace stems {

namespace {

struct SourceRow {
	std::string uuid;
	std::string name;
};

std::vector<SourceRow> list_audio_sources()
{
	std::vector<SourceRow> out;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			auto *v = static_cast<std::vector<SourceRow> *>(param);
			if (!src || (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
				return true;
			const char *u = obs_source_get_uuid(src);
			const char *n = obs_source_get_name(src);
			v->push_back(SourceRow{u ? u : "", n ? n : ""});
			return true;
		},
		&out);
	return out;
}

QLabel *hint(const QString &text)
{
	auto *l = new QLabel(text);
	l->setWordWrap(true);
	l->setStyleSheet(QStringLiteral("color: palette(mid);"));
	return l;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(tr("Audio Stems Recorder"));
	setModal(true);
	resize(860, 600);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);

	lbl_active_note_ = new QLabel(tr("A capture session is running. Changes apply to the next session."));
	lbl_active_note_->setStyleSheet(QStringLiteral("color: #e5a00d;"));
	lbl_active_note_->setVisible(false);
	root->addWidget(lbl_active_note_);

	auto *row = new QHBoxLayout();
	row->setSpacing(10);
	root->addLayout(row, 1);

	nav_list_ = new QListWidget();
	nav_list_->setFixedWidth(190);
	nav_list_->setSelectionMode(QAbstractItemView::SingleSelection);
	nav_list_->addItem(tr("General"));
	nav_list_->addItem(tr("Audio Sources"));
	nav_list_->addItem(tr("Processing"));
	nav_list_->setStyleSheet(QStringLiteral("QListWidget::item { padding: 6px 6px; }"));
	row->addWidget(nav_list_);

	stack_ = new QStackedWidget();
	stack_->addWidget(build_general_page());
	stack_->addWidget(build_sources_page());
	stack_->addWidget(build_processing_page());
	row->addWidget(stack_, 1);

	connect(nav_list_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
	nav_list_->setCurrentRow(1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	reload_sources();
}

QWidget *SettingsDialog::build_general_page()
{
	auto *page = new QWidget();
	auto *lay = new QVBoxLayout(page);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(10);

	{
		auto *group = new QGroupBox(tr("Start automatically"));
		auto *g = new QVBoxLayout(group);
		chk_recording_ = new QCheckBox(tr("When recording starts (follows pause/resume)"));
		chk_streaming_ = new QCheckBox(tr("When streaming starts"));
		g->addWidget(chk_recording_);
		g->addWidget(chk_streaming_);
		g->addWidget(hint(tr("Manual capture and markers are available from the \"Audio Stems\" dock and "
				     "from Settings > Hotkeys.")));
		lay->addWidget(group);
	}

	{
		auto *group = new QGroupBox(tr("Location"));
		auto *g = new QVBoxLayout(group);
		chk_next_to_recordings_ = new QCheckBox(tr("Save stems next to OBS recordings (\"<recording>_stems\" folder)"));
		g->addWidget(chk_next_to_recordings_);

		auto *rowDir = new QHBoxLayout();
		rowDir->addWidget(new QLabel(tr("Output folder")));
		edit_output_ = new QLineEdit();
		edit_output_->setPlaceholderText(tr("Select a folder..."));
		rowDir->addWidget(edit_output_, 1);
		btn_browse_output_ = new QPushButton(tr("Browse..."));
		rowDir->addWidget(btn_browse_output_);
		g->addLayout(rowDir);
		connect(btn_browse_output_, &QPushButton::clicked, this, [this] {
			const QString dir =
				QFileDialog::getExistingDirectory(this, tr("Select output folder"), edit_output_->text());
			if (!dir.isEmpty())
				edit_output_->setText(QDir::toNativeSeparators(dir));
		});
		connect(chk_next_to_recordings_, &QCheckBox::toggled, this, [this] { update_enabled_state(); });
		lay->addWidget(group);
	}

	{
		auto *group = new QGroupBox(tr("File format"));
		auto *form = new QFormLayout(group);

		combo_output_format_ = new QComboBox();
		combo_output_format_->addItem(tr("WAV (uncompressed)"), QStringLiteral("wav"));
		combo_output_format_->addItem(tr("FLAC (lossless, needs ffmpeg)"), QStringLiteral("flac"));
		combo_output_format_->addItem(tr("MP3 (needs ffmpeg)"), QStringLiteral("mp3"));
		form->addRow(tr("Format"), combo_output_format_);

		combo_wav_bit_depth_ = new QComboBox();
		combo_wav_bit_depth_->addItem(tr("16-bit PCM"), 16);
		combo_wav_bit_depth_->addItem(tr("24-bit PCM (recommended)"), 24);
		combo_wav_bit_depth_->addItem(tr("32-bit float (no clipping)"), 32);
		form->addRow(tr("Capture bit depth"), combo_wav_bit_depth_);

		spin_mp3_bitrate_ = new QSpinBox();
		spin_mp3_bitrate_->setRange(64, 320);
		spin_mp3_bitrate_->setSingleStep(32);
		spin_mp3_bitrate_->setSuffix(QStringLiteral(" kbps"));
		form->addRow(tr("MP3 bitrate"), spin_mp3_bitrate_);

		chk_keep_wav_ = new QCheckBox(tr("Keep the WAV files after FLAC/MP3 export"));
		form->addRow(QString(), chk_keep_wav_);

		auto *rowFf = new QHBoxLayout();
		edit_ffmpeg_ = new QLineEdit();
		edit_ffmpeg_->setPlaceholderText(tr("Auto-detect from PATH"));
		rowFf->addWidget(edit_ffmpeg_, 1);
		auto *btnFf = new QPushButton(tr("Browse..."));
		rowFf->addWidget(btnFf);
		form->addRow(tr("ffmpeg"), rowFf);
		lbl_ffmpeg_status_ = new QLabel();
		lbl_ffmpeg_status_->setWordWrap(true);
		form->addRow(QString(), lbl_ffmpeg_status_);

		connect(btnFf, &QPushButton::clicked, this, [this] {
#if defined(_WIN32)
			const QString filter = tr("ffmpeg (ffmpeg.exe);;All files (*)");
#else
			const QString filter = tr("All files (*)");
#endif
			const QString f = QFileDialog::getOpenFileName(this, tr("Locate ffmpeg"), edit_ffmpeg_->text(), filter);
			if (!f.isEmpty())
				edit_ffmpeg_->setText(QDir::toNativeSeparators(f));
		});
		connect(edit_ffmpeg_, &QLineEdit::editingFinished, this, [this] { update_ffmpeg_status(); });
		connect(combo_output_format_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
			update_enabled_state();
			update_ffmpeg_status();
		});
		lay->addWidget(group);
	}

	lay->addStretch(1);
	return page;
}

QWidget *SettingsDialog::build_sources_page()
{
	auto *page = new QWidget();
	auto *lay = new QVBoxLayout(page);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(8);

	auto *rowBtns = new QHBoxLayout();
	auto *btnAll = new QPushButton(tr("Select all"));
	auto *btnNone = new QPushButton(tr("Select none"));
	auto *btnRefresh = new QPushButton(tr("Refresh"));
	rowBtns->addWidget(btnAll);
	rowBtns->addWidget(btnNone);
	rowBtns->addWidget(btnRefresh);
	rowBtns->addStretch(1);
	lay->addLayout(rowBtns);

	auto set_all = [this](Qt::CheckState st) {
		for (int r = 0; r < table_sources_->rowCount(); r++)
			if (auto *it = table_sources_->item(r, 0))
				it->setCheckState(st);
	};
	connect(btnAll, &QPushButton::clicked, this, [set_all] { set_all(Qt::Checked); });
	connect(btnNone, &QPushButton::clicked, this, [set_all] { set_all(Qt::Unchecked); });
	connect(btnRefresh, &QPushButton::clicked, this, [this] {
		settings_ = get_settings(); // keep current edits
		reload_sources();
		apply_selection_from_settings();
	});

	table_sources_ = new QTableWidget();
	table_sources_->setColumnCount(3);
	table_sources_->setHorizontalHeaderLabels({tr("Record"), tr("Source"), tr("File name (optional)")});
	table_sources_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	table_sources_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	table_sources_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	table_sources_->verticalHeader()->setVisible(false);
	table_sources_->setSelectionMode(QAbstractItemView::NoSelection);
	table_sources_->setEditTriggers(QAbstractItemView::AllEditTriggers);
	table_sources_->setAlternatingRowColors(true);
	table_sources_->setShowGrid(false);
	lay->addWidget(table_sources_, 1);

	chk_use_aliases_ = new QCheckBox(tr("Use the custom file names above"));
	lay->addWidget(chk_use_aliases_);
	chk_post_fader_ = new QCheckBox(tr("Post-fader: apply each source's OBS volume slider and mute"));
	lay->addWidget(chk_post_fader_);
	lay->addWidget(hint(tr("Off (default) records the clean source after its filters, independent of the mixer "
			       "volume. Muted sources are always recorded as silence so stems stay in sync.")));

	auto *group = new QGroupBox(tr("Also record OBS mixer tracks (full mixes)"));
	auto *grid = new QHBoxLayout(group);
	for (int i = 0; i < 6; i++) {
		chk_tracks_[i] = new QCheckBox(tr("Track %1").arg(i + 1));
		grid->addWidget(chk_tracks_[i]);
	}
	grid->addStretch(1);
	lay->addWidget(group);

	return page;
}

QWidget *SettingsDialog::build_processing_page()
{
	auto *page = new QWidget();
	auto *lay = new QVBoxLayout(page);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(10);

	lay->addWidget(hint(tr("Processing runs in the background after a session ends and never affects the "
			       "live stream. Stems are always padded to identical length and kept sample-aligned.")));

	{
		auto *group = new QGroupBox(tr("Trim silence"));
		auto *g = new QVBoxLayout(group);
		chk_trim_ = new QCheckBox(tr("Trim leading and trailing silence"));
		g->addWidget(chk_trim_);
		auto *rowTrim = new QHBoxLayout();
		rowTrim->addWidget(new QLabel(tr("Threshold (dBFS)")));
		spin_trim_thr_ = new QDoubleSpinBox();
		spin_trim_thr_->setRange(-90.0, -1.0);
		spin_trim_thr_->setDecimals(1);
		rowTrim->addWidget(spin_trim_thr_);
		rowTrim->addWidget(new QLabel(tr("Lead (ms)")));
		spin_lead_ms_ = new QSpinBox();
		spin_lead_ms_->setRange(0, 10000);
		rowTrim->addWidget(spin_lead_ms_);
		rowTrim->addWidget(new QLabel(tr("Trail (ms)")));
		spin_trail_ms_ = new QSpinBox();
		spin_trail_ms_->setRange(0, 10000);
		rowTrim->addWidget(spin_trail_ms_);
		rowTrim->addStretch(1);
		g->addLayout(rowTrim);
		g->addWidget(hint(tr("Trimming breaks alignment with the video; the removed offset of every stem is "
				     "written to session.json (trimmed_start_seconds).")));
		lay->addWidget(group);
	}

	{
		auto *group = new QGroupBox(tr("Normalize"));
		auto *g = new QVBoxLayout(group);
		chk_norm_ = new QCheckBox(tr("Normalize loudness (gated RMS)"));
		g->addWidget(chk_norm_);
		auto *rowNorm = new QHBoxLayout();
		rowNorm->addWidget(new QLabel(tr("Target (dBFS)")));
		spin_norm_target_ = new QDoubleSpinBox();
		spin_norm_target_->setRange(-60.0, -1.0);
		spin_norm_target_->setDecimals(1);
		rowNorm->addWidget(spin_norm_target_);
		chk_limiter_ = new QCheckBox(tr("Never exceed -0.3 dBFS peak"));
		rowNorm->addWidget(chk_limiter_);
		rowNorm->addStretch(1);
		g->addLayout(rowNorm);
		lay->addWidget(group);
	}

	{
		auto *group = new QGroupBox(tr("Metadata"));
		auto *g = new QVBoxLayout(group);
		chk_sidecar_ = new QCheckBox(tr("Write session.json (stems, offsets, levels, markers)"));
		chk_scene_markers_ = new QCheckBox(tr("Add a marker on every scene change"));
		g->addWidget(chk_sidecar_);
		g->addWidget(chk_scene_markers_);
		g->addWidget(hint(tr("Markers are also saved as markers.txt (Audacity label track format).")));
		lay->addWidget(group);
	}

	connect(chk_trim_, &QCheckBox::toggled, this, [this] { update_enabled_state(); });
	connect(chk_norm_, &QCheckBox::toggled, this, [this] { update_enabled_state(); });

	lay->addStretch(1);
	return page;
}

void SettingsDialog::reload_sources()
{
	table_sources_->setRowCount(0);
	for (const auto &src : list_audio_sources()) {
		const int row = table_sources_->rowCount();
		table_sources_->insertRow(row);

		auto *itCheck = new QTableWidgetItem();
		itCheck->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
		itCheck->setCheckState(Qt::Unchecked);
		itCheck->setData(Qt::UserRole, QString::fromStdString(src.uuid));
		itCheck->setData(Qt::UserRole + 1, QString::fromUtf8(src.name.c_str()));
		table_sources_->setItem(row, 0, itCheck);

		auto *itName = new QTableWidgetItem(QString::fromUtf8(src.name.empty() ? "(unnamed)" : src.name.c_str()));
		itName->setFlags(Qt::ItemIsEnabled);
		table_sources_->setItem(row, 1, itName);

		auto *itAlias = new QTableWidgetItem();
		itAlias->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable);
		table_sources_->setItem(row, 2, itAlias);
	}
}

void SettingsDialog::set_settings(const Settings &s)
{
	settings_ = s;
	chk_recording_->setChecked(s.trigger_recording);
	chk_streaming_->setChecked(s.trigger_streaming);
	chk_next_to_recordings_->setChecked(s.save_next_to_recordings);
	edit_output_->setText(QString::fromUtf8(s.output_dir.c_str()));

	int idx = combo_output_format_->findData(QString::fromStdString(s.output_format));
	combo_output_format_->setCurrentIndex(idx < 0 ? 0 : idx);
	idx = combo_wav_bit_depth_->findData(s.wav_bit_depth);
	combo_wav_bit_depth_->setCurrentIndex(idx < 0 ? 1 : idx);
	spin_mp3_bitrate_->setValue(s.mp3_bitrate_kbps);
	chk_keep_wav_->setChecked(s.keep_wav_after_export);
	edit_ffmpeg_->setText(QString::fromUtf8(s.ffmpeg_path.c_str()));

	chk_use_aliases_->setChecked(s.use_source_aliases);
	chk_post_fader_->setChecked(s.post_fader);
	for (int i = 0; i < 6; i++)
		chk_tracks_[i]->setChecked((s.track_mask & (1u << i)) != 0);

	chk_trim_->setChecked(s.trim_silence);
	spin_trim_thr_->setValue(s.trim_threshold_dbfs);
	spin_lead_ms_->setValue(s.trim_lead_ms);
	spin_trail_ms_->setValue(s.trim_trail_ms);
	chk_norm_->setChecked(s.normalize_audio);
	spin_norm_target_->setValue(s.normalize_target_dbfs);
	chk_limiter_->setChecked(s.normalize_limiter);
	chk_sidecar_->setChecked(s.write_sidecar_json);
	chk_scene_markers_->setChecked(s.record_scene_markers);

	apply_selection_from_settings();
	update_enabled_state();
	update_ffmpeg_status();
}

void SettingsDialog::set_sessions_active(bool active)
{
	lbl_active_note_->setVisible(active);
}

void SettingsDialog::apply_selection_from_settings()
{
	for (int row = 0; row < table_sources_->rowCount(); row++) {
		auto *it = table_sources_->item(row, 0);
		const std::string uuid = it->data(Qt::UserRole).toString().toStdString();
		const std::string name = it->data(Qt::UserRole + 1).toString().toUtf8().toStdString();

		bool selected = false;
		for (const auto &sel : settings_.selected_sources) {
			if ((!sel.uuid.empty() && sel.uuid == uuid) || (sel.uuid.empty() && sel.name == name)) {
				selected = true;
				break;
			}
		}
		it->setCheckState(selected ? Qt::Checked : Qt::Unchecked);

		QString alias;
		for (const auto &p : settings_.source_aliases)
			if (p.first == uuid)
				alias = QString::fromUtf8(p.second.c_str());
		if (auto *itAlias = table_sources_->item(row, 2))
			itAlias->setText(alias);
	}
}

void SettingsDialog::update_enabled_state()
{
	const QString fmt = combo_output_format_->currentData().toString();
	const bool exported = fmt != QStringLiteral("wav");
	spin_mp3_bitrate_->setEnabled(fmt == QStringLiteral("mp3"));
	chk_keep_wav_->setEnabled(exported);
	edit_ffmpeg_->setEnabled(exported);

	const bool custom_dir = !chk_next_to_recordings_->isChecked();
	edit_output_->setEnabled(true); // still used for streaming/manual sessions and as fallback
	btn_browse_output_->setEnabled(true);
	edit_output_->setToolTip(custom_dir ? QString()
					    : tr("Used when no recording folder is known (streaming / manual capture)."));

	spin_trim_thr_->setEnabled(chk_trim_->isChecked());
	spin_lead_ms_->setEnabled(chk_trim_->isChecked());
	spin_trail_ms_->setEnabled(chk_trim_->isChecked());
	spin_norm_target_->setEnabled(chk_norm_->isChecked());
	chk_limiter_->setEnabled(chk_norm_->isChecked());
}

void SettingsDialog::update_ffmpeg_status()
{
	if (combo_output_format_->currentData().toString() == QStringLiteral("wav")) {
		lbl_ffmpeg_status_->setText(QString());
		return;
	}
	const std::string found = find_ffmpeg(edit_ffmpeg_->text().trimmed().toUtf8().toStdString());
	if (found.empty())
		lbl_ffmpeg_status_->setText(
			tr("<span style='color:#e04848'>ffmpeg not found &mdash; stems will be kept as WAV.</span>"));
	else
		lbl_ffmpeg_status_->setText(
			tr("Using: %1").arg(QString::fromUtf8(found.c_str()).toHtmlEscaped()));
}

Settings SettingsDialog::get_settings() const
{
	Settings s = settings_;
	s.trigger_recording = chk_recording_->isChecked();
	s.trigger_streaming = chk_streaming_->isChecked();
	s.save_next_to_recordings = chk_next_to_recordings_->isChecked();
	s.output_dir = edit_output_->text().trimmed().toUtf8().toStdString();
	s.output_format = combo_output_format_->currentData().toString().toStdString();
	s.wav_bit_depth = combo_wav_bit_depth_->currentData().toInt();
	s.mp3_bitrate_kbps = spin_mp3_bitrate_->value();
	s.keep_wav_after_export = chk_keep_wav_->isChecked();
	s.ffmpeg_path = edit_ffmpeg_->text().trimmed().toUtf8().toStdString();

	s.use_source_aliases = chk_use_aliases_->isChecked();
	s.post_fader = chk_post_fader_->isChecked();
	s.track_mask = 0;
	for (int i = 0; i < 6; i++)
		if (chk_tracks_[i]->isChecked())
			s.track_mask |= 1u << i;

	s.trim_silence = chk_trim_->isChecked();
	s.trim_threshold_dbfs = (float)spin_trim_thr_->value();
	s.trim_lead_ms = spin_lead_ms_->value();
	s.trim_trail_ms = spin_trail_ms_->value();
	s.normalize_audio = chk_norm_->isChecked();
	s.normalize_target_dbfs = (float)spin_norm_target_->value();
	s.normalize_limiter = chk_limiter_->isChecked();
	s.write_sidecar_json = chk_sidecar_->isChecked();
	s.record_scene_markers = chk_scene_markers_->isChecked();

	// Rebuild selection from the table, but keep selections for sources that
	// are not present right now (e.g. defined in another scene collection).
	std::set<std::string> listed;
	std::vector<SelectedSource> selected;
	std::vector<std::pair<std::string, std::string>> aliases;
	for (int row = 0; row < table_sources_->rowCount(); row++) {
		auto *it = table_sources_->item(row, 0);
		if (!it)
			continue;
		const std::string uuid = it->data(Qt::UserRole).toString().toStdString();
		const std::string name = it->data(Qt::UserRole + 1).toString().toUtf8().toStdString();
		listed.insert(uuid);
		if (it->checkState() == Qt::Checked)
			selected.push_back(SelectedSource{uuid, name});
		if (auto *itAlias = table_sources_->item(row, 2)) {
			const QString a = itAlias->text().trimmed();
			if (!a.isEmpty() && !uuid.empty())
				aliases.emplace_back(uuid, a.toUtf8().toStdString());
		}
	}
	for (const auto &sel : settings_.selected_sources)
		if (!sel.uuid.empty() && !listed.count(sel.uuid))
			selected.push_back(sel);
	for (const auto &p : settings_.source_aliases)
		if (!listed.count(p.first))
			aliases.push_back(p);

	s.selected_sources = std::move(selected);
	s.source_aliases = std::move(aliases);
	return s;
}

} // namespace stems
