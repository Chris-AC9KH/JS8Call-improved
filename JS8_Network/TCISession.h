#pragma once

#include "TCIClient.h"

#include <QObject>
#include <QUrl>

class TCISession final : public QObject
{
  Q_OBJECT

public:
  explicit TCISession(QObject *parent = nullptr);

  TCIClient *client() const { return m_client; }

  Q_SLOT void connectToServer(QUrl const &url);
  Q_SLOT void disconnectFromServer();

  QUrl url() const { return m_url; }

private:
  TCIClient *m_client = nullptr;
  QUrl m_url;
};