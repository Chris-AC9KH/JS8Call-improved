#include "TCISession.h"

#include <QDebug>
#include <QMetaObject>
#include <QThread>

TCISession::TCISession(QObject *parent)
    : QObject(parent),
      m_client(new TCIClient(this))
{
}

void TCISession::connectToServer(QUrl const &url)
{
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(
        this,
        [this, url]() {
            connectToServer(url);
        },
        Qt::QueuedConnection);
    return;
  }

  if (m_url == url && m_client && (m_client->isConnected() || m_client->isConnecting())) {
    m_client->emitReadyIfReady();
    return;
  }

  m_url = url;

  m_client->connectToServer(url);
}

void TCISession::disconnectFromServer()
{
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(
        this,
        [this]() {
            disconnectFromServer();
        },
        Qt::QueuedConnection);
    return;
  }

  if (!m_client)
    return;

  m_client->disconnectFromServer();
}