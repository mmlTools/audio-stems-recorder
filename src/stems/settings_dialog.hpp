#pragma once

#include <QDialog>

#include <array>

#include "settings.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace stems {

class SettingsDialog : public QDialog {
	Q_OBJECT
public:
	explicit SettingsDialog(QWidget *parent = nullptr);

	void set_settings(const Settings &s);
	void set_sessions_active(bool active);
	Settings get_settings() const;

private:
	QWidget *build_general_page();
	QWidget *build_sources_page();
	QWidget *build_processing_page();
	void reload_sources();
	void apply_selection_from_settings();
	void update_enabled_state();
	void update_ffmpeg_status();

	Settings settings_;

	// General
	QCheckBox *chk_recording_ = nullptr;
	QCheckBox *chk_streaming_ = nullptr;
	QCheckBox *chk_next_to_recordings_ = nullptr;
	QLineEdit *edit_output_ = nullptr;
	QPushButton *btn_browse_output_ = nullptr;
	QComboBox *combo_output_format_ = nullptr;
	QComboBox *combo_wav_bit_depth_ = nullptr;
	QSpinBox *spin_mp3_bitrate_ = nullptr;
	QCheckBox *chk_keep_wav_ = nullptr;
	QLineEdit *edit_ffmpeg_ = nullptr;
	QLabel *lbl_ffmpeg_status_ = nullptr;
	QLabel *lbl_active_note_ = nullptr;

	// Sources
	QTableWidget *table_sources_ = nullptr;
	QCheckBox *chk_use_aliases_ = nullptr;
	QCheckBox *chk_post_fader_ = nullptr;
	std::array<QCheckBox *, 6> chk_tracks_{};

	// Processing
	QCheckBox *chk_trim_ = nullptr;
	QDoubleSpinBox *spin_trim_thr_ = nullptr;
	QSpinBox *spin_lead_ms_ = nullptr;
	QSpinBox *spin_trail_ms_ = nullptr;
	QCheckBox *chk_norm_ = nullptr;
	QDoubleSpinBox *spin_norm_target_ = nullptr;
	QCheckBox *chk_limiter_ = nullptr;
	QCheckBox *chk_sidecar_ = nullptr;
	QCheckBox *chk_scene_markers_ = nullptr;

	QListWidget *nav_list_ = nullptr;
	QStackedWidget *stack_ = nullptr;
};

} // namespace stems
