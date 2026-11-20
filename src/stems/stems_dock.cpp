#include "stems_dock.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace stems {

StemsDock::StemsDock(QWidget *parent) : QWidget(parent)
{
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->setSpacing(6);

	status_ = new QLabel(this);
	status_->setTextFormat(Qt::RichText);
	status_->setWordWrap(true);
	status_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	status_->setTextInteractionFlags(Qt::NoTextInteraction);
	lay->addWidget(status_, 1);

	auto *row = new QHBoxLayout();
	row->setSpacing(4);
	btn_manual_ = new QPushButton(tr("Start Capture"), this);
	btn_marker_ = new QPushButton(tr("Add Marker"), this);
	row->addWidget(btn_manual_);
	row->addWidget(btn_marker_);
	lay->addLayout(row);

	auto *row2 = new QHBoxLayout();
	row2->setSpacing(4);
	btn_folder_ = new QPushButton(tr("Open Folder"), this);
	btn_settings_ = new QPushButton(tr("Settings..."), this);
	row2->addWidget(btn_folder_);
	row2->addWidget(btn_settings_);
	lay->addLayout(row2);

	btn_manual_->setToolTip(tr("Capture stems without recording or streaming"));
	btn_marker_->setToolTip(tr("Drop a marker into every running stem session (session.json / markers.txt)"));

	connect(btn_manual_, &QPushButton::clicked, this, [this] {
		if (cb_.toggle_manual)
			cb_.toggle_manual();
		refresh();
	});
	connect(btn_marker_, &QPushButton::clicked, this, [this] {
		if (cb_.add_marker)
			cb_.add_marker();
	});
	connect(btn_settings_, &QPushButton::clicked, this, [this] {
		if (cb_.open_settings)
			cb_.open_settings();
	});
	connect(btn_folder_, &QPushButton::clicked, this, [this] {
		if (cb_.open_folder)
			cb_.open_folder();
	});

	timer_ = new QTimer(this);
	timer_->setInterval(250);
	connect(timer_, &QTimer::timeout, this, &StemsDock::refresh);
}

void StemsDock::set_callbacks(Callbacks cb)
{
	cb_ = std::move(cb);
	timer_->start();
	refresh();
}

void StemsDock::detach()
{
	timer_->stop();
	cb_ = Callbacks{};
	btn_manual_->setEnabled(false);
	btn_marker_->setEnabled(false);
	btn_settings_->setEnabled(false);
	btn_folder_->setEnabled(false);
}

void StemsDock::refresh()
{
	if (!cb_.model || !isVisible())
		return;
	const DockModel m = cb_.model();

	QString html;
	if (m.rows.empty()) {
		html = tr("<b>Idle</b> &mdash; stems start automatically with recording/streaming.");
	} else {
		for (const auto &r : m.rows) {
			const QString color = r.warn ? QStringLiteral("#e5a00d") : QStringLiteral("#e04848");
			html += QStringLiteral("<div><span style='color:%1'>&#9679;</span> <b>%2</b> %3</div>")
					.arg(color, QString::fromUtf8(r.title.c_str()).toHtmlEscaped(),
					     QString::fromUtf8(r.detail.c_str()).toHtmlEscaped());
		}
	}
	if (m.finalizing > 0)
		html += tr("<div><i>Finalizing %1 session(s) in background...</i></div>").arg(m.finalizing);
	status_->setText(html);

	btn_manual_->setText(m.manual_running ? tr("Stop Capture") : tr("Start Capture"));
	btn_marker_->setEnabled(m.any_running);
}

} // namespace stems
