/*****************************************************************
File:        event_dispatcher.h
Version:     1.0
Author:
start date:
Description: 

Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef _EVENT_DISPATCHER_H_
#define _EVENT_DISPATCHER_H_

#include <QObject>

#include "singleton.h"

/**
 * @class EventDispatcher
 * @brief 消息分发中心. 所有订阅者都可以将slot函数connect到此处定义的signal上.
 */
class EventDispatcher : public QObject
{
Q_OBJECT
public:
    friend SingletonTemplate<EventDispatcher>;
    virtual ~EventDispatcher();

protected:
    explicit EventDispatcher(QObject * parent = nullptr);

signals:
    
};

#endif //_EVENT_DISPATCHER_H_
