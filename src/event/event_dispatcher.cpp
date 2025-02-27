#include "event_dispatcher.h"

EventDispatcher::EventDispatcher(QObject *parent)
    : QObject(parent)
{
    // 可通过qRegisterMetaType注册自定义类型，方便信号信号槽类型传递
}

EventDispatcher::~EventDispatcher()
{}
