/*****************************************************************
File:        transcode_window.h
Version:     1.0
Author:      cjx
start date:
Description: 媒体格式转换窗口（基于 FFmpegTranscoder）
    功能：
        1. 导入视频（支持多选文件 / 直接导入 DVD 的 VIDEO_TS 目录）
        2. 目标容器（mp4/mkv/avi/mov/ts/mpg/flv）与目标编码器选择（含"仅换封装"流拷贝）
        3. 目标分辨率、码率设置，进度显示与取消
        4. 多个 VOB 段落可合并转换为单个文件

Version history
[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef _TRANSCODE_WINDOW_H
#define _TRANSCODE_WINDOW_H

#ifdef CAN_USE_FFMPEG

#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class FFmpegTranscoder;

class TranscodeWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TranscodeWindow(QWidget *parent = nullptr);
    ~TranscodeWindow() override;

    /** 预填输入文件（供播放窗口"转换格式"入口调用，整碟段落会合并转码） */
    void presetInput(const QStringList &files);

protected:
    void initWidgetInfo();
    void initLayout();
    void setupConnections();
    void closeEvent(QCloseEvent *event) override;

private slots:
    // 输入输出
    void addInputFiles();
    void addDvdFolder();
    void removeSelectedInput();
    void clearInputs();
    void selectOutputFile();
    void onOutputFormatChanged(int index);
    void onCodecOptionChanged();        // 编码方式变化：自动禁用无意义的码率/分辨率控件并给出推荐
    // 转码控制
    void startTranscode();
    void cancelTranscode();
    void onProgress(int percent);
    void onStateInfo(const QString &info);
    void onFinished(bool success, const QString &msg);
    void onError(const QString &error);

private:
    void initCodecOptions();            // 初始化编码器选项并标记可用性
    void updateOutputSuffix();          // 按目标容器同步输出文件后缀
    QString suggestedOutputPath() const;
    void updateControlState(bool running);
    void probeSourceResolution();       // 探测首个输入的原始分辨率
    void updateOptionStates();          // 刷新控件可用状态与推荐码率
    int recommendedBitrate(int height) const;   // 按分辨率高度推荐视频码率
    QString bitrateHintFor(int height) const;   // 该分辨率对应的常用码率范围说明

    // 输入
    QListWidget *m_inputList_;
    QPushButton *m_addFileBtn_;
    QPushButton *m_addDvdBtn_;
    QPushButton *m_removeBtn_;
    QPushButton *m_clearBtn_;
    // 输出
    QLineEdit *m_outputEdit_;
    QPushButton *m_outputBtn_;
    QComboBox *m_formatCbx_;
    // 编码参数
    QComboBox *m_videoCodecCbx_;
    QComboBox *m_audioCodecCbx_;
    QComboBox *m_resolutionCbx_;
    QCheckBox *m_keepAspectCbx_;
    QSpinBox *m_videoBitrateSpin_;
    QComboBox *m_audioBitrateCbx_;
    QLabel *m_hintLabel_;               // 当前模式说明与推荐码率提示
    // 控制与状态
    QPushButton *m_startBtn_;
    QPushButton *m_cancelBtn_;
    QProgressBar *m_progressBar_;
    QLabel *m_statusLabel_;

    QStringList m_inputFiles_;          // 待转换的输入文件（按列表顺序）
    QString m_lastOpenDir_;             // 上次打开的目录
    int m_sourceWidth_ = 0;             // 源视频分辨率（用于推荐码率）
    int m_sourceHeight_ = 0;
    bool m_bitrateEdited_ = false;      // 用户是否手动改过视频码率
    bool m_running_ = false;
    FFmpegTranscoder *m_transcoder_ = nullptr;
};

#endif // CAN_USE_FFMPEG

#endif // _TRANSCODE_WINDOW_H
