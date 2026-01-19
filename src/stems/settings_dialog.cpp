#include "settings_dialog.hpp"

#include <QBoxLayout>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QListWidget>
#include <QStackedWidget>
#include <QAbstractItemView>
#include <QFrame>
#include <QWidget>

#include <obs-module.h>

#include <vector>

namespace stems
{

	static void enumerate_audio_sources(std::vector<obs_source_t *> &out)
	{
		out.clear();
		obs_enum_sources(
			[](void *param, obs_source_t *src)
			{
				auto *vec = static_cast<std::vector<obs_source_t *> *>(param);
				if (!src)
					return true;
				uint32_t flags = obs_source_get_output_flags(src);
				if ((flags & OBS_SOURCE_AUDIO) == 0)
					return true;
				obs_source_t *ref = obs_source_get_ref(src);
				if (ref)
					vec->push_back(ref);
				return true;
			},
			&out);
	}

	SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(tr("Audio Stems Recorder"));
		setModal(true);
		resize(820, 560);

		auto *root = new QVBoxLayout();
		root->setContentsMargins(12, 12, 12, 12);
		root->setSpacing(10);
		setLayout(root);

		auto *contentRow = new QHBoxLayout();
		contentRow->setContentsMargins(0, 0, 0, 0);
		contentRow->setSpacing(10);
		root->addLayout(contentRow, 1);

		auto *navWrap = new QWidget();
		navWrap->setObjectName("navWrap");
		auto *navLay = new QVBoxLayout();
		navLay->setContentsMargins(8, 8, 8, 8);
		navLay->setSpacing(8);
		navWrap->setLayout(navLay);
		navWrap->setFixedWidth(230);

		nav_list_ = new QListWidget();
		nav_list_->setObjectName("navList");
		nav_list_->setSelectionMode(QAbstractItemView::SingleSelection);
		nav_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		nav_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
		nav_list_->setUniformItemSizes(true);
		nav_list_->addItem(tr("General"));
		nav_list_->addItem(tr("Audio Sources"));
		nav_list_->addItem(tr("Processing"));
		navLay->addWidget(nav_list_, 1);

		contentRow->addWidget(navWrap);

		auto *panelWrap = new QWidget();
		panelWrap->setObjectName("panelStack");
		auto *panelLay = new QVBoxLayout();
		panelLay->setContentsMargins(12, 12, 12, 12);
		panelLay->setSpacing(10);
		panelWrap->setLayout(panelLay);

		stack_ = new QStackedWidget();
		panelLay->addWidget(stack_, 1);

		contentRow->addWidget(panelWrap, 1);

		apply_dialog_style_();

		{
			auto *page = new QWidget();
			auto *lay = new QVBoxLayout();
			lay->setContentsMargins(0, 0, 0, 0);
			lay->setSpacing(10);
			page->setLayout(lay);

			{
				auto *group = new QGroupBox(tr("Triggers"));
				auto *g = new QVBoxLayout();
				g->setContentsMargins(10, 12, 10, 10);
				g->setSpacing(8);
				group->setLayout(g);

				chk_recording_ = new QCheckBox(tr("Capture when Recording starts/stops"));
				chk_streaming_ = new QCheckBox(tr("Capture when Streaming starts/stops"));
				g->addWidget(chk_recording_);
				g->addWidget(chk_streaming_);

				lay->addWidget(group);
			}

			{
				auto *group = new QGroupBox(tr("Output"));
				auto *g = new QVBoxLayout();
				g->setContentsMargins(10, 12, 10, 10);
				g->setSpacing(8);
				group->setLayout(g);

				auto *rowDir = new QHBoxLayout();
				rowDir->setSpacing(8);
				rowDir->addWidget(new QLabel(tr("Output folder")));

				edit_output_ = new QLineEdit();
				edit_output_->setPlaceholderText(tr("Select a folder..."));
				rowDir->addWidget(edit_output_, 1);

				auto *btnBrowseDir = new QPushButton(tr("Browse"));
				btnBrowseDir->setObjectName("primaryBtn");
				connect(btnBrowseDir, &QPushButton::clicked, this, &SettingsDialog::on_browse_output);
				rowDir->addWidget(btnBrowseDir);

				g->addLayout(rowDir);
				lay->addWidget(group);
			}

			lay->addStretch(1);
			stack_->addWidget(page);
		}

		{
			auto *page = new QWidget();
			auto *src = new QVBoxLayout();
			src->setContentsMargins(0, 0, 0, 0);
			src->setSpacing(10);
			page->setLayout(src);

			auto *rowBtns = new QHBoxLayout();
			rowBtns->setSpacing(8);

			auto *btnAll = new QPushButton(tr("Select all"));
			btnAll->setObjectName("secondaryBtn");

			auto *btnNone = new QPushButton(tr("Select none"));
			btnNone->setObjectName("secondaryBtn");

			connect(btnAll, &QPushButton::clicked, this, &SettingsDialog::on_select_all_sources);
			connect(btnNone, &QPushButton::clicked, this, &SettingsDialog::on_select_none_sources);

			rowBtns->addWidget(btnAll);
			rowBtns->addWidget(btnNone);
			rowBtns->addStretch(1);
			src->addLayout(rowBtns);

			chk_use_aliases_ = new QCheckBox(tr("Use custom file names (aliases)"));
			src->addWidget(chk_use_aliases_);

			table_sources_ = new QTableWidget();
			table_sources_->setObjectName("sourcesTable");
			table_sources_->setColumnCount(3);
			table_sources_->setHorizontalHeaderLabels({tr("Record"), tr("Source"), tr("Alias (optional)")});
			table_sources_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
			table_sources_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
			table_sources_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
			table_sources_->verticalHeader()->setVisible(false);
			table_sources_->setSelectionMode(QAbstractItemView::NoSelection);
			table_sources_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
			table_sources_->setAlternatingRowColors(true);
			table_sources_->setShowGrid(false);
			src->addWidget(table_sources_, 1);

			stack_->addWidget(page);
		}

		{
			auto *page = new QWidget();
			auto *proc = new QVBoxLayout();
			proc->setContentsMargins(0, 0, 0, 0);
			proc->setSpacing(10);
			page->setLayout(proc);

			{
				auto *group = new QGroupBox(tr("Trim"));
				auto *g = new QVBoxLayout();
				g->setContentsMargins(10, 12, 10, 10);
				g->setSpacing(8);
				group->setLayout(g);

				chk_trim_ = new QCheckBox(tr("Trim silence"));
				g->addWidget(chk_trim_);

				auto *rowTrim = new QHBoxLayout();
				rowTrim->setSpacing(8);

				rowTrim->addWidget(new QLabel(tr("Threshold (dBFS)")));
				spin_trim_thr_ = new QDoubleSpinBox();
				spin_trim_thr_->setRange(-90.0, -1.0);
				spin_trim_thr_->setDecimals(1);
				rowTrim->addWidget(spin_trim_thr_);

				rowTrim->addWidget(new QLabel(tr("Lead (ms)")));
				spin_lead_ms_ = new QSpinBox();
				spin_lead_ms_->setRange(0, 5000);
				rowTrim->addWidget(spin_lead_ms_);

				rowTrim->addWidget(new QLabel(tr("Trail (ms)")));
				spin_trail_ms_ = new QSpinBox();
				spin_trail_ms_->setRange(0, 5000);
				rowTrim->addWidget(spin_trail_ms_);

				rowTrim->addStretch(1);
				g->addLayout(rowTrim);

				proc->addWidget(group);
			}

			{
				auto *group = new QGroupBox(tr("Normalize"));
				auto *g = new QVBoxLayout();
				g->setContentsMargins(10, 12, 10, 10);
				g->setSpacing(8);
				group->setLayout(g);

				chk_norm_ = new QCheckBox(tr("Normalize audio"));
				g->addWidget(chk_norm_);

				auto *rowNorm = new QHBoxLayout();
				rowNorm->setSpacing(8);

				rowNorm->addWidget(new QLabel(tr("Target (dBFS)")));
				spin_norm_target_ = new QDoubleSpinBox();
				spin_norm_target_->setRange(-60.0, -1.0);
				spin_norm_target_->setDecimals(1);
				rowNorm->addWidget(spin_norm_target_);

				chk_limiter_ = new QCheckBox(tr("Limiter (prevent clipping)"));
				rowNorm->addWidget(chk_limiter_);

				rowNorm->addStretch(1);
				g->addLayout(rowNorm);

				proc->addWidget(group);
			}

			{
				auto *group = new QGroupBox(tr("Metadata"));
				auto *g = new QVBoxLayout();
				g->setContentsMargins(10, 12, 10, 10);
				g->setSpacing(8);
				group->setLayout(g);

				chk_sidecar_ = new QCheckBox(tr("Write session.json sidecar"));
				chk_scene_markers_ = new QCheckBox(tr("Record scene change markers"));
				g->addWidget(chk_sidecar_);
				g->addWidget(chk_scene_markers_);

				proc->addWidget(group);
			}

			proc->addStretch(1);
			stack_->addWidget(page);
		}

		connect(nav_list_, &QListWidget::currentRowChanged, this, [this](int row)
				{
		if (row >= 0 && row < stack_->count())
			stack_->setCurrentIndex(row); });

		nav_list_->setCurrentRow(1);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		root->addWidget(buttons);

		reload_sources();
	}

	void SettingsDialog::reload_sources()
	{
		table_sources_->setRowCount(0);
		std::vector<obs_source_t *> sources;
		enumerate_audio_sources(sources);

		for (obs_source_t *src : sources)
		{
			const char *name = obs_source_get_name(src);
			const char *uuid = obs_source_get_uuid(src);
			int row = table_sources_->rowCount();
			table_sources_->insertRow(row);

			auto *itCheck = new QTableWidgetItem();
			itCheck->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
			itCheck->setCheckState(Qt::Unchecked);
			itCheck->setData(Qt::UserRole, QString::fromUtf8(uuid ? uuid : ""));
			table_sources_->setItem(row, 0, itCheck);

			auto *itName = new QTableWidgetItem(QString::fromUtf8(name ? name : "(unnamed)"));
			itName->setFlags(Qt::ItemIsEnabled);
			table_sources_->setItem(row, 1, itName);

			auto *itAlias = new QTableWidgetItem();
			itAlias->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable);
			table_sources_->setItem(row, 2, itAlias);

			obs_source_release(src);
		}
	}

	void SettingsDialog::set_settings(const Settings &s)
	{
		settings_ = s;
		chk_recording_->setChecked(settings_.trigger_recording);
		chk_streaming_->setChecked(settings_.trigger_streaming);
		edit_output_->setText(QString::fromUtf8(settings_.output_dir.c_str()));
		chk_trim_->setChecked(settings_.trim_silence);
		spin_trim_thr_->setValue(settings_.trim_threshold_dbfs);
		spin_lead_ms_->setValue(settings_.trim_lead_ms);
		spin_trail_ms_->setValue(settings_.trim_trail_ms);
		chk_norm_->setChecked(settings_.normalize_audio);
		spin_norm_target_->setValue(settings_.normalize_target_dbfs);
		chk_limiter_->setChecked(settings_.normalize_limiter);
		chk_sidecar_->setChecked(settings_.write_sidecar_json);
		chk_scene_markers_->setChecked(settings_.record_scene_markers);
		chk_use_aliases_->setChecked(settings_.use_source_aliases);
		apply_selection_from_settings();
	}

	void SettingsDialog::apply_selection_from_settings()
	{
		auto alias_for = [&](const std::string &uuid) -> std::string
		{
			for (const auto &p : settings_.source_aliases)
			{
				if (p.first == uuid)
					return p.second;
			}
			return {};
		};

		for (int row = 0; row < table_sources_->rowCount(); row++)
		{
			auto *it = table_sources_->item(row, 0);
			const std::string uuid = it->data(Qt::UserRole).toString().toUtf8().constData();
			bool selected = false;
			for (const auto &u : settings_.selected_source_uuids)
			{
				if (uuid == u)
				{
					selected = true;
					break;
				}
			}
			it->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
			auto *itAlias = table_sources_->item(row, 2);
			if (itAlias)
				itAlias->setText(QString::fromUtf8(alias_for(uuid).c_str()));
		}
	}

	Settings SettingsDialog::get_settings() const
	{
		Settings s;
		s.trigger_recording = chk_recording_->isChecked();
		s.trigger_streaming = chk_streaming_->isChecked();
		s.output_dir = edit_output_->text().toUtf8().constData();
		s.trim_silence = chk_trim_->isChecked();
		s.trim_threshold_dbfs = (float)spin_trim_thr_->value();
		s.trim_lead_ms = spin_lead_ms_->value();
		s.trim_trail_ms = spin_trail_ms_->value();
		s.normalize_audio = chk_norm_->isChecked();
		s.normalize_target_dbfs = (float)spin_norm_target_->value();
		s.normalize_limiter = chk_limiter_->isChecked();
		s.write_sidecar_json = chk_sidecar_->isChecked();
		s.record_scene_markers = chk_scene_markers_->isChecked();
		s.use_source_aliases = chk_use_aliases_->isChecked();

		s.selected_source_uuids.clear();
		s.source_aliases.clear();

		for (int row = 0; row < table_sources_->rowCount(); row++)
		{
			auto *it = table_sources_->item(row, 0);
			if (!it)
				continue;
			const QString uuid = it->data(Qt::UserRole).toString();
			if (uuid.isEmpty())
				continue;
			if (it->checkState() == Qt::Checked)
				s.selected_source_uuids.push_back(uuid.toUtf8().constData());
			auto *itAlias = table_sources_->item(row, 2);
			if (itAlias)
			{
				QString a = itAlias->text().trimmed();
				if (!a.isEmpty())
					s.source_aliases.emplace_back(uuid.toUtf8().constData(), a.toUtf8().constData());
			}
		}

		return s;
	}

	void SettingsDialog::on_browse_output()
	{
		QString dir = QFileDialog::getExistingDirectory(this, tr("Select output folder"), edit_output_->text());
		if (!dir.isEmpty())
			edit_output_->setText(dir);
	}

	void SettingsDialog::on_select_all_sources()
	{
		for (int row = 0; row < table_sources_->rowCount(); row++)
		{
			auto *it = table_sources_->item(row, 0);
			if (it)
				it->setCheckState(Qt::Checked);
		}
	}

	void SettingsDialog::on_select_none_sources()
	{
		for (int row = 0; row < table_sources_->rowCount(); row++)
		{
			auto *it = table_sources_->item(row, 0);
			if (it)
				it->setCheckState(Qt::Unchecked);
		}
	}

	void SettingsDialog::apply_dialog_style_()
	{
		setStyleSheet(R"QSS(
QDialog {
	background: #1b1d21;
	color: #e8e8e8;
}

#navWrap {
	background: #23262b;
	border: 1px solid rgba(255,255,255,0.08);
	border-radius: 6px;
}

#panelStack {
	background: #1f2126;
	border: 1px solid rgba(255,255,255,0.08);
	border-radius: 6px;
}

#navList {
	background: transparent;
	border: none;
	padding: 6px;
	outline: none;
}
#navList::item {
	padding: 5px 5px;
	margin: 4px 4px;
	border-radius: 6px;
	color: rgba(255,255,255,0.88);
}
#navList::item:hover {
	background: rgba(255,255,255,0.06);
}
#navList::item:selected {
	background: rgba(90,140,255,0.22);
	color: #ffffff;
}

QGroupBox {
	border: 1px solid rgba(255,255,255,0.08);
	border-radius: 3px;
	margin-top: 10px;
}
QGroupBox::title {
	subcontrol-origin: margin;
	left: 10px;
	padding: 0 6px;
	color: rgba(255,255,255,0.86);
	font-weight: 600;
}

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
	background: rgba(255,255,255,0.06);
	border: 1px solid rgba(255,255,255,0.10);
	border-radius: 3px;
	padding: 7px 10px;
	min-height: 30px;
	color: #ffffff;
	selection-background-color: rgba(90,140,255,0.35);
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
	border-color: rgba(90,140,255,0.65);
	background: rgba(255,255,255,0.08);
}

QPushButton {
	border-radius: 3px;
	padding: 8px 14px;
	border: 1px solid rgba(255,255,255,0.14);
	background: rgba(255,255,255,0.09);
	color: #ffffff;
}
QPushButton:hover {
	background: rgba(255,255,255,0.13);
	border-color: rgba(255,255,255,0.20);
}
QPushButton:pressed {
	background: rgba(255,255,255,0.07);
}

QPushButton#primaryBtn {
	background: rgba(90, 140, 255, 0.90);
	border-color: rgba(90, 140, 255, 0.95);
	color: #0b1020;
	font-weight: 700;
}
QPushButton#primaryBtn:hover {
	background: rgba(90, 140, 255, 0.98);
}
QPushButton#primaryBtn:pressed {
	background: rgba(90, 140, 255, 0.80);
}

QPushButton#secondaryBtn {
	background: rgba(255,255,255,0.10);
	border-color: rgba(255,255,255,0.18);
	font-weight: 600;
}
QPushButton#secondaryBtn:hover {
	background: rgba(255,255,255,0.15);
	border-color: rgba(255,255,255,0.22);
}

QCheckBox { spacing: 8px; }
QCheckBox::indicator {
	width: 16px;
	height: 16px;
	border-radius: 3px;
	border: 1px solid rgba(255,255,255,0.20);
	background: rgba(255,255,255,0.06);
}
QCheckBox::indicator:checked {
	background: rgba(90, 140, 255, 0.90);
	border-color: rgba(90, 140, 255, 0.95);
}

QTableWidget#sourcesTable {
	background: rgba(255,255,255,0.03);
	border: 1px solid rgba(255,255,255,0.08);
	border-radius: 3px;
	gridline-color: rgba(255,255,255,0.06);
	alternate-background-color: rgba(255,255,255,0.04);
	color: #ffffff;
}
QHeaderView::section {
	background: rgba(255,255,255,0.07);
	border: none;
	border-bottom: 1px solid rgba(255,255,255,0.10);
	padding: 8px 10px;
	font-weight: 700;
	color: rgba(255,255,255,0.92);
}
QTableWidget::item {
	padding: 8px 10px;
	color: rgba(255,255,255,0.92);
}
QTableWidget QLineEdit {
	background: rgba(0,0,0,0.20);
	border: 1px solid rgba(90,140,255,0.55);
	border-radius: 3px;
	color: #ffffff;
}

QDialogButtonBox QPushButton {
	min-width: 94px;
}
)QSS");
	}

} // namespace stems
