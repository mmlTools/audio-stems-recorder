#pragma once

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QTimer;

namespace stems {

struct DockRow {
	std::string title;
	std::string detail;
	bool warn = false;
};

struct DockModel {
	std::vector<DockRow> rows;
	bool manual_running = false;
	bool any_running = false;
	size_t finalizing = 0;
};

// Small status dock: live session state plus quick actions. Polls the plugin
// on the UI thread at 4 Hz; it never touches audio threads.
class StemsDock : public QWidget {
public:
	struct Callbacks {
		std::function<DockModel()> model;
		std::function<void()> toggle_manual;
		std::function<void()> add_marker;
		std::function<void()> open_settings;
		std::function<void()> open_folder;
	};

	explicit StemsDock(QWidget *parent = nullptr);
	void set_callbacks(Callbacks cb);
	// Called on OBS exit: stop polling and drop callbacks into the plugin.
	void detach();

private:
	void refresh();

	Callbacks cb_;
	QTimer *timer_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *btn_manual_ = nullptr;
	QPushButton *btn_marker_ = nullptr;
	QPushButton *btn_settings_ = nullptr;
	QPushButton *btn_folder_ = nullptr;
};

} // namespace stems
