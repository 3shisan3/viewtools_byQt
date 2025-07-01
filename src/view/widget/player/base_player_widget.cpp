#include "base_player_widget.h"

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

PlayerWidgetBase::PlayerWidgetBase(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<PlayerWidgetBase::PlayState>("PlayerWidgetBase::PlayState");
}

PlayerWidgetBase::~PlayerWidgetBase()
{
    // 资源通过Qt父子关系自动释放
}

void PlayerWidgetBase::errorInfoShow(const QString &errorInfo)
{
    emit playStateChanged(StoppedState); // 停止播放状态

    // 弹窗提示错误信息
    QDialog *dialog = new QDialog;
    dialog->setWindowTitle("Error");
    dialog->setModal(true);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true); // 关闭时自动删除
    dialog->setWindowModality(Qt::WindowModal);

    QLabel *label = new QLabel(errorInfo, dialog);
    QVBoxLayout *layout = new QVBoxLayout(dialog);
    layout->addWidget(label);

    QPushButton *okButton = new QPushButton("OK", dialog);
    connect(okButton, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(okButton);

    dialog->exec(); // 显示模态对话框
}