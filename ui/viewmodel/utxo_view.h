// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <QObject>
#include <QQmlListProperty>
#include "model/wallet_model.h"

class UtxoItem : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString amount       READ amount     NOTIFY changed)
    Q_PROPERTY(QString height       READ height     NOTIFY changed)
    Q_PROPERTY(QString maturity     READ maturity   NOTIFY changed)
    Q_PROPERTY(QString status       READ status     NOTIFY changed)
    Q_PROPERTY(QString type         READ type       NOTIFY changed)
public:

    UtxoItem() = default;
    UtxoItem(const beam::Coin& coin);

    QString amount() const;
    QString height() const;
    QString maturity() const;
    QString status() const;
    QString type() const;

signals:
    void changed();

private:
    beam::Coin _coin;
};

class UtxoViewModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QQmlListProperty<UtxoItem> allUtxos READ getAllUtxos NOTIFY allUtxoChanged)
    Q_PROPERTY(QString currentHeight READ getCurrentHeight NOTIFY stateChanged)
    Q_PROPERTY(QString currentStateHash READ getCurrentStateHash NOTIFY stateChanged)

public:
    UtxoViewModel();
    virtual ~UtxoViewModel();
    QQmlListProperty<UtxoItem> getAllUtxos();
    QString getCurrentHeight() const;
    QString getCurrentStateHash() const;
public slots:
    void onAllUtxoChanged(const std::vector<beam::Coin>& utxos);
    void onStatus(const WalletStatus& status);
signals:
    void allUtxoChanged();
    void stateChanged();
private:

    QList<UtxoItem*> _allUtxos;
    QString _currentHeight;
    QString _currentStateHash;
    WalletModel& _model;
};