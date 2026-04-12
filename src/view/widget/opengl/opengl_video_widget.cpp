#include "opengl_video_widget.h"

#ifdef OPENGL_ENABLE

#include <QCoreApplication>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QThread>

// ==================== 着色器源码 ====================

/**
 * @brief 顶点着色器
 * 
 * 功能：处理顶点坐标和纹理坐标的传递
 * - a_position: 顶点位置属性（屏幕坐标，范围-1到1）
 * - a_texCoord: 纹理坐标属性（范围0到1）
 * - v_texCoord: 传递给片段着色器的纹理坐标
 */
static const char *vertexShaderSource =
    "attribute vec4 a_position;\n"           // 顶点位置输入
    "attribute vec2 a_texCoord;\n"           // 纹理坐标输入
    "varying vec2 v_texCoord;\n"             // 传递给片段着色器
    "void main() {\n"
    "    gl_Position = a_position;\n"        // 设置顶点位置
    "    v_texCoord = a_texCoord;\n"         // 传递纹理坐标
    "}\n";

/**
 * @brief RGB模式片段着色器
 * 
 * 功能：直接从RGB纹理采样输出颜色
 * 适用场景：输入已经是RGB格式的图像
 */
static const char *fragmentShaderSourceRGB =
    "uniform sampler2D u_texture;\n"         // RGB纹理采样器
    "varying vec2 v_texCoord;\n"             // 纹理坐标
    "void main() {\n"
    "    vec4 color = texture2D(u_texture, v_texCoord);\n"
    "    gl_FragColor = color;\n"            // 直接采样输出
    "}\n";

/**
 * @brief YUV转RGB片段着色器 - BT.601标准
 * 
 * BT.601公式（标清电视标准）：
 * R = Y + 1.402 * (V - 128)
 * G = Y - 0.344 * (U - 128) - 0.714 * (V - 128)
 * B = Y + 1.772 * (U - 128)
 * 
 * 适用范围：DVD、VCD、标清电视、老旧视频文件
 * 兼容性：最好，几乎所有播放器都支持
 */
static const char *fragmentShaderSourceYUV_BT601 =
    "uniform sampler2D u_textureY;\n"        // Y平面纹理采样器
    "uniform sampler2D u_textureU;\n"        // U平面纹理采样器
    "uniform sampler2D u_textureV;\n"        // V平面纹理采样器
    "varying vec2 v_texCoord;\n"             // 纹理坐标
    "void main() {\n"
    "    float y = texture2D(u_textureY, v_texCoord).r;\n"      // 获取Y值（亮度）
    "    float u = texture2D(u_textureU, v_texCoord).r - 0.5;\n" // 获取U值（色度），归一化到[-0.5,0.5]
    "    float v = texture2D(u_textureV, v_texCoord).r - 0.5;\n" // 获取V值（色度），归一化到[-0.5,0.5]
    "    // BT.601标准YUV转RGB公式\n"
    "    float r = y + 1.402 * v;\n"         // 红色分量计算
    "    float g = y - 0.344 * u - 0.714 * v;\n"  // 绿色分量计算
    "    float b = y + 1.772 * u;\n"         // 蓝色分量计算
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

/**
 * @brief YUV转RGB片段着色器 - BT.709标准
 * 
 * BT.709公式（高清电视标准）：
 * R = Y + 1.28033 * (V - 128)
 * G = Y - 0.21482 * (U - 128) - 0.38059 * (V - 128)
 * B = Y + 2.12798 * (U - 128)
 * 
 * 适用范围：720p、1080p、4K、蓝光、现代视频
 * 色彩准确性：更高，色域更广
 */
static const char *fragmentShaderSourceYUV_BT709 =
    "uniform sampler2D u_textureY;\n"
    "uniform sampler2D u_textureU;\n"
    "uniform sampler2D u_textureV;\n"
    "varying vec2 v_texCoord;\n"
    "void main() {\n"
    "    float y = texture2D(u_textureY, v_texCoord).r;\n"
    "    float u = texture2D(u_textureU, v_texCoord).r - 0.5;\n"
    "    float v = texture2D(u_textureV, v_texCoord).r - 0.5;\n"
    "    // BT.709标准YUV转RGB公式（适用于高清视频）\n"
    "    float r = y + 1.28033 * v;\n"       // 红色分量系数略有不同
    "    float g = y - 0.21482 * u - 0.38059 * v;\n"  // 绿色分量系数调整
    "    float b = y + 2.12798 * u;\n"       // 蓝色分量系数调整
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

// ==================== OpenGLVideoWidget 实现 ====================

OpenGLVideoWidget::OpenGLVideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    // 设置OpenGL上下文格式
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setVersion(2, 1);              // OpenGL 2.1，兼容性最好
    format.setSwapInterval(1);            // 启用垂直同步，避免画面撕裂
    setFormat(format);
    
    // 设置Widget属性，优化渲染性能
    setAttribute(Qt::WA_OpaquePaintEvent);      // 不透明绘制事件，跳过背景绘制
    setAttribute(Qt::WA_NoSystemBackground);    // 无系统背景，减少绘制开销
    setAutoFillBackground(false);               // 禁止自动填充背景
    
    // 启动帧率计时器
    m_fpsTimer.start();
    
    qDebug() << "OpenGLVideoWidget created with OpenGL 2.1 context";
}

OpenGLVideoWidget::~OpenGLVideoWidget()
{
    // 确保在销毁前释放OpenGL资源
    cleanupGL();
}

void OpenGLVideoWidget::checkGLError(const char* location)
{
    GLenum err;
    bool hasError = false;
    while ((err = glGetError()) != GL_NO_ERROR) {
        qWarning() << "OpenGL error at" << location << ":" << err;
        hasError = true;
    }
    
    if (hasError) {
        // 输出额外的调试信息
        qDebug() << "OpenGL context:" << QOpenGLContext::currentContext();
        qDebug() << "Widget size:" << size();
        qDebug() << "Render mode:" << m_renderMode;
        qDebug() << "Has frame:" << m_hasFrame;
        qDebug() << "Frame size:" << m_currentFrame.size();
    }
}

bool OpenGLVideoWidget::isOpenGLAvailable()
{
    // 运行时检测OpenGL可用性
    static int available = -1;  // -1:未检测, 0:不可用, 1:可用
    
    if (available == -1) {
        // 尝试创建临时OpenGL上下文进行检测
        QOpenGLContext context;
        if (context.create()) {
            available = 1;
            qDebug() << "OpenGL available, version:" 
                     << context.format().majorVersion() << "."
                     << context.format().minorVersion();
        } else {
            available = 0;
            qWarning() << "OpenGL not available on this system";
        }
    }
    
    return available == 1;
}

void OpenGLVideoWidget::setColorSpace(ColorSpace space)
{
    if (m_colorSpace == space) return;
    
    m_colorSpace = space;
    qDebug() << "Color space changed to:" << (space == COLOR_BT601 ? "BT.601" : "BT.709");
    
    // 重新创建YUV着色器以应用新的色彩空间
    if (m_initialized && m_programYUV) {
        delete m_programYUV;
        m_programYUV = nullptr;
        createShaders();
    }
}

QString OpenGLVideoWidget::getRenderMode() const
{
    switch (m_renderMode) {
        case ModeRGB: return "OpenGL RGB";
        case ModeYUV: 
            return QString("OpenGL YUV (%1)").arg(m_colorSpace == COLOR_BT601 ? "BT.601" : "BT.709");
        default: return "None";
    }
}

OpenGLVideoWidget::ColorSpace OpenGLVideoWidget::autoSelectColorSpace(int width, int height)
{
    // 根据分辨率自动选择色彩空间
    // 720p（1280x720）及以上使用BT.709，以下使用BT.601
    if (width >= 1280 || height >= 720) {
        return COLOR_BT709;
    } else {
        return COLOR_BT601;
    }
}

bool OpenGLVideoWidget::createShaders()
{
    // ==================== 创建RGB着色器 ====================
    m_programRGB = new QOpenGLShaderProgram(this);
    
    // 添加顶点着色器
    if (!m_programRGB->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource)) {
        qWarning() << "RGB: Vertex shader compilation failed:" << m_programRGB->log();
        return false;
    }
    
    // 添加片段着色器
    if (!m_programRGB->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSourceRGB)) {
        qWarning() << "RGB: Fragment shader compilation failed:" << m_programRGB->log();
        return false;
    }
    
    // 链接着色器程序
    if (!m_programRGB->link()) {
        qWarning() << "RGB: Shader program link failed:" << m_programRGB->log();
        return false;
    }
    
    // ==================== 创建YUV着色器 ====================
    m_programYUV = new QOpenGLShaderProgram(this);
    
    // 添加顶点着色器（与RGB模式共用）
    if (!m_programYUV->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource)) {
        qWarning() << "YUV: Vertex shader compilation failed:" << m_programYUV->log();
        return false;
    }
    
    // 根据色彩空间选择对应的片段着色器
    const char* yuvFragmentShader = (m_colorSpace == COLOR_BT601) 
        ? fragmentShaderSourceYUV_BT601 
        : fragmentShaderSourceYUV_BT709;
    
    if (!m_programYUV->addShaderFromSourceCode(QOpenGLShader::Fragment, yuvFragmentShader)) {
        qWarning() << "YUV: Fragment shader compilation failed:" << m_programYUV->log();
        return false;
    }
    
    // 链接着色器程序
    if (!m_programYUV->link()) {
        qWarning() << "YUV: Shader program link failed:" << m_programYUV->log();
        return false;
    }
    
    qDebug() << "Shaders created successfully - YUV using" 
             << (m_colorSpace == COLOR_BT601 ? "BT.601" : "BT.709");
    
    return true;
}

void OpenGLVideoWidget::initializeGL()
{
    // 初始化OpenGL函数
    initializeOpenGLFunctions();
    
    // 获取OpenGL信息用于调试
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    
    qDebug() << "OpenGL Vendor:" << (vendor ? reinterpret_cast<const char*>(vendor) : "Unknown");
    qDebug() << "OpenGL Renderer:" << (renderer ? reinterpret_cast<const char*>(renderer) : "Unknown");
    qDebug() << "OpenGL Version:" << (version ? reinterpret_cast<const char*>(version) : "Unknown");
    
    // 设置清除颜色为黑色
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    
    // 禁用深度测试（2D渲染不需要）
    glDisable(GL_DEPTH_TEST);
    
    // 启用2D纹理
    glEnable(GL_TEXTURE_2D);
    
    // 初始化纹理
    initRGBTextures();
    initYUVTextures();
    
    // 创建着色器程序
    if (!createShaders()) {
        qWarning() << "Failed to create shaders, video rendering may not work correctly";
        return;
    }
    
    // ==================== 设置顶点数据 ====================
    // 顶点数据格式：[位置(x,y), 纹理坐标(u,v)]
    // 使用标准纹理坐标：原点在左下角
    float vertices[] = {
        // 位置(x,y)        纹理坐标(u,v)
        -1.0f, -1.0f,       0.0f, 1.0f,   // 左下角
         1.0f, -1.0f,       1.0f, 1.0f,   // 右下角
         1.0f,  1.0f,       1.0f, 0.0f,   // 右上角
        -1.0f,  1.0f,       0.0f, 0.0f    // 左上角
    };
    
    // 创建并填充顶点缓冲区
    m_vertexBuffer.create();
    m_vertexBuffer.bind();
    m_vertexBuffer.allocate(vertices, sizeof(vertices));
    
    // 索引数据（两个三角形组成矩形）
    GLuint indices[] = {
        0, 1, 2,   // 第一个三角形（右下部分）
        0, 2, 3    // 第二个三角形（左上部分）
    };
    
    // 创建并填充索引缓冲区
    m_indexBuffer.create();
    m_indexBuffer.bind();
    m_indexBuffer.allocate(indices, sizeof(indices));
    
    // 释放缓冲区绑定
    m_vertexBuffer.release();
    m_indexBuffer.release();
    
    m_initialized = true;
    
    checkGLError("initializeGL end");
    
    qDebug() << "OpenGL video widget initialized successfully";
}

void OpenGLVideoWidget::initRGBTextures()
{
    // 生成RGB纹理
    glGenTextures(1, &m_textureRGB);
    glBindTexture(GL_TEXTURE_2D, m_textureRGB);
    
    // 设置纹理参数
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);       // 缩小使用线性滤波，平滑
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);       // 放大使用线性滤波
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);    // S方向边缘钳位
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);    // T方向边缘钳位
    
    // 先分配一个空的纹理（1x1像素，黑色）
    unsigned char blackPixel[4] = {0, 0, 0, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, 
                 GL_RGBA, GL_UNSIGNED_BYTE, blackPixel);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    checkGLError("initRGBTextures");
}

void OpenGLVideoWidget::initYUVTextures()
{
    // 生成YUV三个平面的纹理
    glGenTextures(1, &m_textureY);
    glGenTextures(1, &m_textureU);
    glGenTextures(1, &m_textureV);
    
    // 为每个纹理设置相同的参数
    for (GLuint tex : {m_textureY, m_textureU, m_textureV}) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLVideoWidget::resizeGL(int w, int h)
{
    // 更新视口大小
    glViewport(0, 0, w, h);
    
    qDebug() << "OpenGL viewport resized to:" << w << "x" << h;
}

void OpenGLVideoWidget::paintGL()
{
    // 检查初始化状态
    if (!m_initialized) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    
    // 清除颜色缓冲区
    glClear(GL_COLOR_BUFFER_BIT);
    
    // ==================== 处理待处理的YUV数据 ====================
    {
        QMutexLocker locker(&m_yuvMutex);
        if (m_pendingYUV.valid) {
            // 更新纹理数据
            updateYUVTextures(reinterpret_cast<const uint8_t*>(m_pendingYUV.yData.constData()),
                  reinterpret_cast<const uint8_t*>(m_pendingYUV.uData.constData()),
                  reinterpret_cast<const uint8_t*>(m_pendingYUV.vData.constData()),
                  m_pendingYUV.yLinesize,
                  m_pendingYUV.uLinesize,
                  m_pendingYUV.vLinesize,
                  m_pendingYUV.width,
                  m_pendingYUV.height);
            m_pendingYUV.valid = false;
        }
    }
    
    // ==================== 获取当前帧数据并更新纹理 ====================
    QMutexLocker locker(&m_frameMutex);
    
    if (!m_hasFrame) {
        return;
    }
    
    // ==================== 如果是RGB模式，更新RGB纹理 ====================
    if (m_renderMode == ModeRGB && !m_currentFrame.isNull()) {
        // 复制一份图像数据，避免临时对象问题
        QImage textureImage = m_currentFrame.copy();
        
        // 转换为OpenGL兼容的格式
        // QImage::Format_RGBA8888 是Qt 5.4+支持的格式，与OpenGL的GL_RGBA匹配
        if (textureImage.format() != QImage::Format_RGBA8888) {
            textureImage = textureImage.convertToFormat(QImage::Format_RGBA8888);
        }
        
        // 确保图像数据连续存储
        if (textureImage.bytesPerLine() != textureImage.width() * 4) {
            textureImage = textureImage.copy(); // 强制深拷贝，确保数据连续
        }
        
        m_frameWidth = textureImage.width();
        m_frameHeight = textureImage.height();
        
        glBindTexture(GL_TEXTURE_2D, m_textureRGB);
        
        // 使用 GL_RGBA 格式，与 QImage::Format_RGBA8888 匹配
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 
                     textureImage.width(), textureImage.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, textureImage.constBits());
        
        glBindTexture(GL_TEXTURE_2D, 0);
        
        checkGLError("RGB texture update");
    }
    
    // ==================== 选择并绑定着色器 ====================
    QOpenGLShaderProgram *program = nullptr;
    
    if (m_renderMode == ModeRGB && m_programRGB) {
        program = m_programRGB;
    } else if (m_renderMode == ModeYUV && m_programYUV) {
        program = m_programYUV;
    } else {
        return;  // 无效的渲染模式
    }
    
    if (!program->bind()) {
        qWarning() << "Failed to bind shader program";
        return;
    }
    
    // ==================== 绑定纹理 ====================
    if (m_renderMode == ModeRGB) {
        // RGB模式：单个纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_textureRGB);
        program->setUniformValue("u_texture", 0);
        
        // 设置纹理参数
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (m_renderMode == ModeYUV) {
        // YUV模式：三个纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_textureY);
        program->setUniformValue("u_textureY", 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_textureU);
        program->setUniformValue("u_textureU", 1);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_textureV);
        program->setUniformValue("u_textureV", 2);
    }
    
    // ==================== 绑定顶点缓冲区并绘制 ====================
    m_vertexBuffer.bind();
    m_indexBuffer.bind();
    
    // 获取顶点属性位置
    int posLocation = program->attributeLocation("a_position");
    int texCoordLocation = program->attributeLocation("a_texCoord");
    
    // 设置顶点位置属性
    if (posLocation >= 0) {
        program->enableAttributeArray(posLocation);
        // 位置数据：每顶点2个float，偏移0，步长4个float
        program->setAttributeBuffer(posLocation, GL_FLOAT, 0, 2, 4 * sizeof(float));
    }
    
    // 设置纹理坐标属性
    if (texCoordLocation >= 0) {
        program->enableAttributeArray(texCoordLocation);
        // 纹理坐标：每顶点2个float，偏移2个float，步长4个float
        program->setAttributeBuffer(texCoordLocation, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    }
    
    // 绘制三角形
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    
    checkGLError("glDrawElements");
    
    // 清理属性数组
    if (posLocation >= 0) {
        program->disableAttributeArray(posLocation);
    }
    if (texCoordLocation >= 0) {
        program->disableAttributeArray(texCoordLocation);
    }
    
    // 释放缓冲区
    m_vertexBuffer.release();
    m_indexBuffer.release();
    
    // 释放着色器
    program->release();
    
    // 更新帧率统计
    updateFpsStats();
}

void OpenGLVideoWidget::updateTextureFromImage(const QImage &image)
{
    if (image.isNull()) return;
    
    // 确保OpenGL上下文是当前的
    makeCurrent();
    
    // 转换为OpenGL兼容的格式
    QImage textureImage = image.copy();
    
    // 确保格式为RGBA8888
    if (textureImage.format() != QImage::Format_RGBA8888) {
        textureImage = textureImage.convertToFormat(QImage::Format_RGBA8888);
    }
    
    // 记录尺寸信息
    m_frameWidth = textureImage.width();
    m_frameHeight = textureImage.height();
    m_texWidth = textureImage.width();
    m_texHeight = textureImage.height();
    
    // 更新纹理数据
    glBindTexture(GL_TEXTURE_2D, m_textureRGB);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_texWidth, m_texHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, textureImage.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    
    checkGLError("updateTextureFromImage");
    
    doneCurrent();
}

void OpenGLVideoWidget::updateYUVTextures(const uint8_t *y_data, const uint8_t *u_data, const uint8_t *v_data,
                                           int y_linesize, int u_linesize, int v_linesize,
                                           int width, int height)
{
    if (!y_data || !u_data || !v_data) return;
    
    // 确保OpenGL上下文是当前的
    makeCurrent();
    
    // 记录尺寸信息
    m_yWidth = width;
    m_yHeight = height;
    m_uvWidth = width / 2;   // YUV420格式中UV平面尺寸为宽度的一半
    m_uvHeight = height / 2;  // YUV420格式中UV平面尺寸为高度的一半
    
    // 上传Y平面（亮度）
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_yWidth, m_yHeight, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, y_data);
    
    // 上传U平面（色度蓝色差）
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_uvWidth, m_uvHeight, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, u_data);
    
    // 上传V平面（色度红色差）
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_uvWidth, m_uvHeight, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, v_data);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    checkGLError("updateYUVTextures");
    
    doneCurrent();
}

void OpenGLVideoWidget::updateFrame(const QImage &frame)
{
    // 检查初始化状态
    if (!m_initialized) {
        qDebug() << "OpenGL not initialized, deferring frame update";
        return;
    }
    
    if (frame.isNull()) return;
    
    {
        QMutexLocker locker(&m_frameMutex);
        // 深拷贝图像数据，确保数据生命周期
        m_currentFrame = frame.copy();
        m_renderMode = ModeRGB;
        m_hasFrame = true;
    }
    
    // 请求重绘（在GUI线程中执行）
    update();
}

void OpenGLVideoWidget::updateFrameYUV(const uint8_t *y_data, const uint8_t *u_data, const uint8_t *v_data,
                                        int y_linesize, int u_linesize, int v_linesize,
                                        int width, int height)
{
    // 检查初始化状态
    if (!m_initialized) {
        qDebug() << "OpenGL not initialized, deferring YUV frame update";
        return;
    }
    
    if (!y_data || !u_data || !v_data) return;
    
    // 复制YUV数据（因为可能在另一个线程中，需要保存副本）
    {
        QMutexLocker locker(&m_yuvMutex);
        m_pendingYUV.yData = QByteArray(reinterpret_cast<const char*>(y_data), height * y_linesize);
        m_pendingYUV.uData = QByteArray(reinterpret_cast<const char*>(u_data), (height/2) * u_linesize);
        m_pendingYUV.vData = QByteArray(reinterpret_cast<const char*>(v_data), (height/2) * v_linesize);
        m_pendingYUV.yLinesize = y_linesize;
        m_pendingYUV.uLinesize = u_linesize;
        m_pendingYUV.vLinesize = v_linesize;
        m_pendingYUV.width = width;
        m_pendingYUV.height = height;
        m_pendingYUV.valid = true;
    }
    
    {
        QMutexLocker locker(&m_frameMutex);
        m_renderMode = ModeYUV;
        m_hasFrame = true;
    }
    
    // 请求重绘
    update();
}

void OpenGLVideoWidget::clear()
{
    QMutexLocker locker(&m_frameMutex);
    m_currentFrame = QImage();
    m_hasFrame = false;
    m_renderMode = ModeNone;
    update();
}

void OpenGLVideoWidget::updateFpsStats()
{
    m_frameCount++;
    
    // 每秒更新一次帧率
    if (m_fpsTimer.elapsed() >= 1000) {
        m_currentFps = m_frameCount * 1000.0f / m_fpsTimer.restart();
        m_frameCount = 0;
        
        // 每10秒输出一次FPS信息（避免日志过多）
        static int logCounter = 0;
        if (++logCounter >= 10) {
            qDebug() << "OpenGL renderer FPS:" << m_currentFps 
                     << "Mode:" << getRenderMode();
            logCounter = 0;
        }
    }
}

void OpenGLVideoWidget::cleanupGL()
{
    // 切换到当前OpenGL上下文
    makeCurrent();
    
    // 释放着色器程序
    if (m_programRGB) {
        delete m_programRGB;
        m_programRGB = nullptr;
    }
    if (m_programYUV) {
        delete m_programYUV;
        m_programYUV = nullptr;
    }
    
    // 删除纹理
    if (m_textureRGB) {
        glDeleteTextures(1, &m_textureRGB);
        m_textureRGB = 0;
    }
    if (m_textureY) {
        glDeleteTextures(1, &m_textureY);
        m_textureY = 0;
    }
    if (m_textureU) {
        glDeleteTextures(1, &m_textureU);
        m_textureU = 0;
    }
    if (m_textureV) {
        glDeleteTextures(1, &m_textureV);
        m_textureV = 0;
    }
    
    // 释放缓冲区
    m_vertexBuffer.destroy();
    m_indexBuffer.destroy();
    
    m_initialized = false;
    
    // 完成当前上下文操作
    doneCurrent();
}

#endif // OPENGL_ENABLE