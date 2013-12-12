#include "mainwindow.h"
#include "aboutwindow.h"
#include "network.h"
#include "ui_mainwindow.h"

#include <string>
#include <map>
#include <set>
#include <vector>
#include <stdexcept>
#include <utility>

#include <QClipboard>
#include <QMutex>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QDebug>
#include <QProcess>
#include <QStringList>
#include <QVBoxLayout>

// Globally visible
ZeroTier::Node::LocalClient *zeroTierClient = (ZeroTier::Node::LocalClient *)0;

// Main window instance for app
static MainWindow *mainWindow = (MainWindow *)0;

static void handleZTMessage(void *arg,unsigned long id,const char *line)
{
	static std::map< unsigned long,std::vector<std::string> > ztReplies;
	static QMutex ztReplies_m;

	ztReplies_m.lock();
	if (*line) {
		ztReplies[id].push_back(std::string(line));
		ztReplies_m.unlock();
	} else { // empty lines conclude transmissions
		std::map< unsigned long,std::vector<std::string> >::iterator r(ztReplies.find(id));
		if (r != ztReplies.end()) {
			MainWindow::ZTMessageEvent *event = new MainWindow::ZTMessageEvent(r->second);
			ztReplies.erase(r);
			ztReplies_m.unlock();
			QCoreApplication::postEvent(mainWindow,event); // must post since this may be another thread
		} else ztReplies_m.unlock();
	}
}

MainWindow::MainWindow(QWidget *parent) :
	QMainWindow(parent),
	ui(new Ui::MainWindow)
{
	ui->setupUi(this);
	this->startTimer(1000); // poll service every second
	this->setEnabled(false); // gets enabled when updates are received
	mainWindow = this;
	this->cyclesSinceResponseFromService = 0;
}

MainWindow::~MainWindow()
{
	delete ui;
	delete zeroTierClient;
	zeroTierClient = (ZeroTier::Node::LocalClient *)0;
	mainWindow = (MainWindow *)0;
}

void MainWindow::timerEvent(QTimerEvent *event)
{
	QMainWindow::timerEvent(event);

	if (!zeroTierClient) {
		std::string dotAuthFile((QDir::homePath() + QDir::separator() + ".zeroTierOneAuthToken").toStdString());
		std::string authToken;
		if (!ZeroTier::Utils::readFile(dotAuthFile.c_str(),authToken)) {
#ifdef __APPLE__
			// Run the little AppleScript hack that asks for admin credentials and
			// then installs the auth token file in the current user's home.
			QString authHelperPath(QCoreApplication::applicationDirPath() + "/../Resources/helpers/mac/ZeroTier One (Authenticate).app/Contents/MacOS/applet");
			if (!QFile::exists(authHelperPath)) {
				// Allow this to also work from the source tree if it's run from there.
				// This is for debugging purposes and shouldn't harm the live release
				// in any way.
				authHelperPath = QCoreApplication::applicationDirPath() + "/../../../../ZeroTierUI/helpers/mac/ZeroTier One (Authenticate).app/Contents/MacOS/applet";
				if (!QFile::exists(authHelperPath)) {
					QMessageBox::critical(this,"Unable to Locate Helper","Unable to locate authorization helper, cannot obtain authentication token.",QMessageBox::Ok,QMessageBox::NoButton);
					QApplication::exit(1);
					return;
				}
			}
			QProcess::execute(authHelperPath,QStringList());
#endif

			if (!ZeroTier::Utils::readFile(dotAuthFile.c_str(),authToken)) {
				QMessageBox::critical(this,"Cannot Authorize","Unable to authorize this user to administrate ZeroTier One.\n\nTo do so manually, copy 'authtoken.secret' from the ZeroTier One home directory to '.zeroTierOneAuthToken' in your home directory and set file modes on this file to only be readable by you (e.g. 0600 on Mac or Linux systems).",QMessageBox::Ok,QMessageBox::NoButton);
				QApplication::exit(1);
				return;
			}
		}

		zeroTierClient = new ZeroTier::Node::LocalClient(authToken.c_str(),0,&handleZTMessage,this);
	}

	// TODO: do something more user-friendly here... or maybe try to restart
	// the service?
	if (++this->cyclesSinceResponseFromService == 3)
		QMessageBox::critical(this,"No Response from Service","The ZeroTier One service does not appear to be running.",QMessageBox::Ok,QMessageBox::NoButton);

	zeroTierClient->send("info");
	zeroTierClient->send("listnetworks");
	zeroTierClient->send("listpeers");
}

void MainWindow::customEvent(QEvent *event)
{
	ZTMessageEvent *m = (ZTMessageEvent *)event; // only one custom event type so far

	if (m->ztMessage.size() == 0)
		return;

	std::vector<std::string> hdr(ZeroTier::Node::LocalClient::splitLine(m->ztMessage[0]));
	if (hdr.size() < 2)
		return;
	if (hdr[0] != "200")
		return;

	this->cyclesSinceResponseFromService = 0;

	if (hdr[1] == "info") {
		if (hdr.size() >= 3)
			this->myAddress = hdr[2].c_str();
		if (hdr.size() >= 4)
			this->myStatus = hdr[3].c_str();
		if (hdr.size() >= 5)
			this->myVersion = hdr[4].c_str();
	} else if (hdr[1] == "listnetworks") {
		std::map< std::string,std::vector<std::string> > byNwid;
		for(unsigned long i=1;i<m->ztMessage.size();++i) {
			std::vector<std::string> l(ZeroTier::Node::LocalClient::splitLine(m->ztMessage[i]));
			// 200 listnetworks <nwid> <name> <status> <config age> <type> <dev> <ips>
			if ((l.size() == 9)&&(l[2].length() == 16))
				byNwid[l[2]] = l;
		}

		std::map< std::string,std::pair<int,Network *> > existingByNwid;
		for(int r=0;r<ui->networkListWidget->count();++r) {
			Network *nw = (Network *)ui->networkListWidget->itemWidget(ui->networkListWidget->item(r));
			existingByNwid[nw->networkId()] = std::make_pair(r,nw);
		}

		for(std::map< std::string,std::pair<int,Network *> >::iterator i(existingByNwid.begin());i!=existingByNwid.end();++i) {
			if (byNwid.count(i->first)) {
				std::vector<std::string> &l = byNwid[i->first];
				i->second.second->setNetworkName(l[3]);
				i->second.second->setStatus(l[4],l[5]);
				i->second.second->setNetworkType(l[6]);
				i->second.second->setNetworkDeviceName(l[7]);
				i->second.second->setIps(l[8]);
			} else {
				delete ui->networkListWidget->takeItem(i->second.first);
			}
		}

		for(std::map< std::string,std::vector<std::string> >::iterator i(byNwid.begin());i!=byNwid.end();++i) {
			if (!existingByNwid.count(i->first)) {
				std::vector<std::string> &l = i->second;
				Network *nw = new Network((QWidget *)0,i->first);
				nw->setNetworkName(l[3]);
				nw->setStatus(l[4],l[5]);
				nw->setNetworkType(l[6]);
				nw->setNetworkDeviceName(l[7]);
				nw->setIps(l[8]);
				QListWidgetItem *item = new QListWidgetItem();
				item->setSizeHint(nw->sizeHint());
				ui->networkListWidget->addItem(item);
				ui->networkListWidget->setItemWidget(item,nw);
			}
		}
	} else if (hdr[1] == "listpeers") {
		this->numPeers = 0;
		for(unsigned long i=1;i<m->ztMessage.size();++i) {
			std::vector<std::string> l(ZeroTier::Node::LocalClient::splitLine(m->ztMessage[i]));
			if ((l.size() >= 5)&&((l[3] != "-")||(l[4] != "-")))
				++this->numPeers; // number of direct peers online -- check for active IPv4 and/or IPv6 address
		}
	}

	if (this->myAddress.size()) {
		QString st(this->myAddress);
		st += "    (";
		st += this->myStatus;
		st += ", v";
		st += this->myVersion;
		st += ", ";
		st += QString::number(this->numPeers);
		st += " peers)";
		while (st.size() < 45)
			st += QChar::Space;
		ui->statusAndAddressButton->setText(st);
	}

	if (this->myStatus == "ONLINE") {
		if (!this->isEnabled())
			this->setEnabled(true);
	} else {
		if (this->isEnabled())
			this->setEnabled(false);
	}
}

void MainWindow::on_joinNetworkButton_clicked()
{
	QString toJoin(ui->networkIdLineEdit->text());
	ui->networkIdLineEdit->setText(QString());

	if (!zeroTierClient) // sanity check
		return;

	if (toJoin.size() != 16) {
		QMessageBox::information(this,"Invalid Network ID","The network ID you entered was not valid. Enter a 16-digit hexadecimal network ID, like '8056c2e21c000001'.",QMessageBox::Ok,QMessageBox::NoButton);
		return;
	}

	zeroTierClient->send((QString("join ") + toJoin).toStdString());
}

void MainWindow::on_actionAbout_triggered()
{
	AboutWindow *about = new AboutWindow(this);
	about->show();
}

void MainWindow::on_networkIdLineEdit_textChanged(const QString &text)
{
	QString newText;
	for(QString::const_iterator i(text.begin());i!=text.end();++i) {
		switch(i->toLatin1()) {
			case '0': newText.append('0'); break;
			case '1': newText.append('1'); break;
			case '2': newText.append('2'); break;
			case '3': newText.append('3'); break;
			case '4': newText.append('4'); break;
			case '5': newText.append('5'); break;
			case '6': newText.append('6'); break;
			case '7': newText.append('7'); break;
			case '8': newText.append('8'); break;
			case '9': newText.append('9'); break;
			case 'a': newText.append('a'); break;
			case 'b': newText.append('b'); break;
			case 'c': newText.append('c'); break;
			case 'd': newText.append('d'); break;
			case 'e': newText.append('e'); break;
			case 'f': newText.append('f'); break;
			case 'A': newText.append('a'); break;
			case 'B': newText.append('b'); break;
			case 'C': newText.append('c'); break;
			case 'D': newText.append('d'); break;
			case 'E': newText.append('e'); break;
			case 'F': newText.append('f'); break;
			default: break;
		}
	}
	if (newText.size() > 16)
		newText.truncate(16);
	ui->networkIdLineEdit->setText(newText);
}

void MainWindow::on_statusAndAddressButton_clicked()
{
	QApplication::clipboard()->setText(this->myAddress);
}