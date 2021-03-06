//
// Aspia Project
// Copyright (C) 2018 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#ifndef HOST__SCREEN_UPDATER_H
#define HOST__SCREEN_UPDATER_H

#include <QObject>

#include "base/macros_magic.h"
#include "proto/desktop_session.pb.h"

namespace host {

class ScreenUpdaterImpl;

class ScreenUpdater : public QObject
{
    Q_OBJECT

public:
    class Delegate
    {
    public:
        virtual ~Delegate() = default;

        virtual void onScreenUpdate(const QByteArray& message) = 0;
    };

    ScreenUpdater(Delegate* delegate, QObject* parent = nullptr);
    ~ScreenUpdater() = default;

public slots:
    bool start(const proto::desktop::Config& config);
    void selectScreen(int64_t screen_id);

protected:
    // QObject implementation.
    void customEvent(QEvent* event) override;

private:
    ScreenUpdaterImpl* impl_ = nullptr;
    Delegate* delegate_;

    DISALLOW_COPY_AND_ASSIGN(ScreenUpdater);
};

} // namespace host

#endif // HOST__SCREEN_UPDATER_H
