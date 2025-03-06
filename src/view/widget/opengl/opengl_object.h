/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        opengl_object.h
Version:     1.0
Author:      cjx
start date: 
Description: 
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef OPENGL_OBJECT_BASE_H_
#define OPENGL_OBJECT_BASE_H_

#include <memory>
#include <QObject>

/* 用宏来区分使用的底图 */


class SsGLObject : public std::enable_shared_from_this<SsGLObject>
{

public:
    SsGLObject();
    virtual ~SsGLObject();


};




#endif  // OPENGL_OBJECT_BASE_H_