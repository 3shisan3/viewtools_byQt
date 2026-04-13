/*****************************************************************
File:        opengl_video_widget.h
Version:     1.1
Author:      cjx
Date:        2026-04-13
Description: OpenGL视频渲染组件，用于高性能4K视频播放
             支持YUV纹理直接渲染，减少CPU到GPU的转换开销
             通过OPENGL_ENABLE宏控制是否启用

Version history
[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]
1             2026-4-13      cjx            create
*****************************************************************/

#ifndef OPENGL_VIDEO_WIDGET_H
#define OPENGL_VIDEO_WIDGET_H

#include <QWidget>
#include <QDateTime>

#ifdef OPENGL_ENABLE

#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QWaitCondition>

/**
 * @brief OpenGL视频渲染Widget
 * 
 * 提供硬件加速的视频渲染，特别针对4K高分辨率视频优化。
 * 支持RGB和YUV两种渲染模式，YUV模式性能更优。
 * 
 * 线程安全说明：
 * - updateFrame/updateFrameYUV 可在任意线程调用（数据会被复制到缓冲区）
 * - 实际OpenGL操作在paintGL中执行（GUI线程）
 */
class OpenGLVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    /**
     * @brief 色彩空间标准枚举
     * 
     * BT.601: 标清电视标准，兼容性最好，适用于老视频
     * BT.709: 高清电视标准，色彩更准确，适用于720p/1080p/4K
     */
    enum ColorSpace {
        COLOR_BT601,    ///< ITU-R BT.601标准（标清，默认）
        COLOR_BT709     ///< ITU-R BT.709标准（高清）
    };
    
    /**
     * @brief 构造函数
     * @param parent 父窗口
     */
    explicit OpenGLVideoWidget(QWidget *parent = nullptr);
    
    /**
     * @brief 析构函数，释放OpenGL资源
     */
    ~OpenGLVideoWidget();

    /**
     * @brief 检查OpenGL是否已完成初始化
     * @return true表示OpenGL已初始化完成，可以接收帧数据
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * @brief 更新视频帧（RGB格式）
     * @param frame QImage格式的视频帧
     * 
     * 使用RGB模式渲染，适用于已经转换为RGB的图像。
     * 线程安全，可在任意线程调用。
     */
    void updateFrame(const QImage &frame);
    
    /**
     * @brief 更新视频帧（YUV420P格式）
     * @param y_data Y平面数据
     * @param u_data U平面数据
     * @param v_data V平面数据
     * @param y_linesize Y平面行步长（字节）
     * @param u_linesize U平面行步长（字节）
     * @param v_linesize V平面行步长（字节）
     * @param width 视频宽度
     * @param height 视频高度
     * 
     * 使用YUV模式直接渲染，GPU完成YUV到RGB转换。
     * 性能优于RGB模式，推荐使用。
     * 线程安全，可在任意线程调用。
     */
    void updateFrameYUV(const uint8_t *y_data, const uint8_t *u_data, const uint8_t *v_data,
                        int y_linesize, int u_linesize, int v_linesize,
                        int width, int height);
    
    /**
     * @brief 检查OpenGL是否可用
     * @return true表示OpenGL可用，false表示不可用
     * 
     * 运行时检测OpenGL上下文创建是否成功。
     * 静态方法，可在创建实例前调用。
     */
    static bool isOpenGLAvailable();
    
    /**
     * @brief 设置色彩空间标准
     * @param space BT.601或BT.709
     * 
     * 影响YUV到RGB转换的色彩准确性。
     * BT.601兼容性最好，BT.709色彩更准确。
     */
    void setColorSpace(ColorSpace space);
    
    /**
     * @brief 获取当前色彩空间
     * @return 当前使用的色彩空间
     */
    ColorSpace getColorSpace() const { return m_colorSpace; }

    /**
     * @brief 清空显示（显示黑屏）
     */
    void clear();

    /**
     * @brief 获取当前渲染帧率
     * @return 当前显示帧率（fps）
     */
    float getCurrentFps() const { return m_currentFps; }
    
    /**
     * @brief 获取当前渲染模式
     * @return 模式描述字符串
     */
    QString getRenderMode() const;

protected:
    /**
     * @brief OpenGL初始化
     * 在第一次显示前由Qt调用，初始化OpenGL资源和着色器
     * Qt会自动调用makeCurrent()
     */
    void initializeGL() override;
    
    /**
     * @brief 窗口大小变化处理
     * @param w 新宽度
     * @param h 新高度
     * Qt会自动调用makeCurrent()
     */
    void resizeGL(int w, int h) override;
    
    /**
     * @brief 每帧渲染
     * 由Qt在需要重绘时调用，执行实际的OpenGL绘制命令
     * Qt会自动调用makeCurrent()
     */
    void paintGL() override;

private:
    /**
     * @brief 渲染模式枚举
     */
    enum RenderMode {
        ModeNone,   ///< 无数据，显示黑屏
        ModeRGB,    ///< RGB纹理模式
        ModeYUV     ///< YUV纹理模式（GPU转换）
    };
    
    /**
     * @brief 初始化RGB纹理
     * 必须在有效的OpenGL上下文中调用
     */
    void initRGBTextures();
    
    /**
     * @brief 初始化YUV纹理
     * 必须在有效的OpenGL上下文中调用
     */
    void initYUVTextures();
    
    /**
     * @brief 清理OpenGL资源
     * 必须在有效的OpenGL上下文中调用
     */
    void cleanupGL();
    
    /**
     * @brief 从QImage更新RGB纹理
     * @param image 源图像
     * 必须在有效的OpenGL上下文中调用
     */
    void updateTextureFromImage(const QImage &image);
    
    /**
     * @brief 更新YUV纹理（内部调用，必须在OpenGL上下文中）
     * @param y_data Y平面数据
     * @param u_data U平面数据
     * @param v_data V平面数据
     * @param y_linesize Y平面行步长
     * @param u_linesize U平面行步长
     * @param v_linesize V平面行步长
     * @param width 视频宽度
     * @param height 视频高度
     * 
     * 注意：此函数必须在有效的OpenGL上下文中调用（如paintGL内部）
     */
    void updateYUVTexturesInternal(const uint8_t *y_data, const uint8_t *u_data, const uint8_t *v_data,
                                   int y_linesize, int u_linesize, int v_linesize,
                                   int width, int height);
    
    /**
     * @brief 创建OpenGL着色器程序
     * 必须在有效的OpenGL上下文中调用
     * @return 是否创建成功
     */
    bool createShaders();
    
    /**
     * @brief 更新帧率统计
     */
    void updateFpsStats();
    
    /**
     * @brief 根据分辨率自动选择色彩空间
     * @param width 视频宽度
     * @param height 视频高度
     * @return 推荐的色彩空间
     */
    ColorSpace autoSelectColorSpace(int width, int height);
    
    /**
     * @brief 检查OpenGL错误并记录
     * @param location 错误发生位置描述
     */
    void checkGLError(const char* location);
    
    /**
     * @brief 计算变换矩阵（保持宽高比）
     * @param frameWidth 视频帧宽度
     * @param frameHeight 视频帧高度
     * @return 变换矩阵
     */
    QMatrix4x4 calculateTransformMatrix(int frameWidth, int frameHeight) const;

    // ==================== OpenGL资源 ====================
    
    QOpenGLShaderProgram *m_programRGB = nullptr;   ///< RGB模式着色器程序
    QOpenGLShaderProgram *m_programYUV = nullptr;   ///< YUV模式着色器程序
    
    GLuint m_textureRGB = 0;    ///< RGB纹理ID
    GLuint m_textureY = 0;      ///< Y平面纹理ID
    GLuint m_textureU = 0;      ///< U平面纹理ID
    GLuint m_textureV = 0;      ///< V平面纹理ID
    
    // ==================== 纹理尺寸 ====================
    
    int m_texWidth = 0;         ///< RGB纹理宽度
    int m_texHeight = 0;        ///< RGB纹理高度
    int m_frameWidth = 0;       ///< 视频帧宽度
    int m_frameHeight = 0;      ///< 视频帧高度
    
    // YUV平面尺寸
    int m_yWidth = 0;           ///< Y平面宽度
    int m_yHeight = 0;          ///< Y平面高度
    int m_uvWidth = 0;          ///< UV平面宽度（YUV420中为宽度/2）
    int m_uvHeight = 0;         ///< UV平面高度（YUV420中为高度/2）
    
    // ==================== 帧数据（线程安全）====================
    
    QImage m_currentFrame;      ///< 当前RGB帧数据
    QMutex m_frameMutex;        ///< 帧数据互斥锁
    
    RenderMode m_renderMode = ModeNone;  ///< 当前渲染模式
    bool m_initialized = false;          ///< OpenGL是否已初始化
    bool m_hasFrame = false;             ///< 是否有有效帧
    
    // ==================== 顶点缓冲区 ====================
    
    QOpenGLBuffer m_vertexBuffer;   ///< 顶点缓冲区（位置+纹理坐标）
    QOpenGLBuffer m_indexBuffer;    ///< 索引缓冲区（三角形索引）
    
    // ==================== 性能统计 ====================
    
    QElapsedTimer m_fpsTimer;       ///< 帧率计时器
    int m_frameCount = 0;           ///< 帧计数器
    float m_currentFps = 0.0f;      ///< 当前帧率
    
    // ==================== 色彩空间 ====================
    
    ColorSpace m_colorSpace = COLOR_BT601;  ///< 当前色彩空间，默认BT.601确保兼容性
    
    // ==================== YUV数据缓冲区（线程安全）====================
    
    /**
     * @brief YUV数据结构，用于跨线程传递
     */
    struct YUVData {
        QByteArray yData;       ///< Y平面数据
        QByteArray uData;       ///< U平面数据
        QByteArray vData;       ///< V平面数据
        int yLinesize = 0;      ///< Y平面行步长
        int uLinesize = 0;      ///< U平面行步长
        int vLinesize = 0;      ///< V平面行步长
        int width = 0;          ///< 视频宽度
        int height = 0;         ///< 视频高度
        bool valid = false;     ///< 数据是否有效
        
        void clear() {
            yData.clear();
            uData.clear();
            vData.clear();
            yLinesize = uLinesize = vLinesize = 0;
            width = height = 0;
            valid = false;
        }
    };
    
    YUVData m_pendingYUV;       ///< 待处理的YUV数据
    QMutex m_yuvMutex;          ///< YUV数据互斥锁
    
    // 标记是否有新的RGB帧需要更新纹理
    bool m_rgbTextureNeedsUpdate = false;
};

#else  // OPENGL_ENABLE未定义

/**
 * @brief 当OPENGL_ENABLE未定义时的空实现
 * 
 * 此时不会编译任何OpenGL相关代码，程序将使用QLabel进行软件渲染。
 * 所有函数均为空实现，确保调用方代码无需修改。
 */
class OpenGLVideoWidget : public QWidget
{
    Q_OBJECT
    
public:
    /**
     * @brief 色彩空间枚举（空实现）
     */
    enum ColorSpace {
        COLOR_BT601,
        COLOR_BT709
    };
    
    explicit OpenGLVideoWidget(QWidget *parent = nullptr) : QWidget(parent) 
    {
        qDebug() << "OpenGL support not compiled (OPENGL_ENABLE not defined)";
    }
    
    bool isInitialized() const { return false; }
    void updateFrame(const QImage &) {}
    void updateFrameYUV(const uint8_t*, const uint8_t*, const uint8_t*, int, int, int, int, int) {}
    static bool isOpenGLAvailable() { return false; }
    void setColorSpace(ColorSpace) {}
    void clear() {}
    float getCurrentFps() const { return 0.0f; }
    QString getRenderMode() const { return "Software (OpenGL disabled)"; }
};

#endif // OPENGL_ENABLE

#endif // OPENGL_VIDEO_WIDGET_H