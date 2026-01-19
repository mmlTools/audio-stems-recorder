#pragma once

#include <QDialog>

#include "settings.hpp"

class QListWidget;
class QTableWidget;
class QLineEdit;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;

namespace stems {

class SettingsDialog : public QDialog {
	Q_OBJECT
public:
	explicit SettingsDialog(QWidget *parent = nullptr);

	void set_settings(const Settings &s);
	Settings get_settings() const;

private slots:
	void on_browse_output();
	void on_select_all_sources();
	void on_select_none_sources();

private:
	void reload_sources();
	void apply_selection_from_settings();

	Settings settings_;

	QCheckBox *chk_recording_ = nullptr;
	QCheckBox *chk_streaming_ = nullptr;
	QCheckBox *chk_trim_ = nullptr;
	QDoubleSpinBox *spin_trim_thr_ = nullptr;
	QSpinBox *spin_lead_ms_ = nullptr;
	QSpinBox *spin_trail_ms_ = nullptr;
	QCheckBox *chk_norm_ = nullptr;
	QDoubleSpinBox *spin_norm_target_ = nullptr;
	QCheckBox *chk_limiter_ = nullptr;
	QCheckBox *chk_sidecar_ = nullptr;
	QCheckBox *chk_scene_markers_ = nullptr;
	QCheckBox *chk_use_aliases_ = nullptr;
	QLineEdit *edit_output_ = nullptr;
	QTableWidget *table_sources_ = nullptr;
};

} // namespace stems
