#ifndef IARU_REGIONS_HPP__
#define IARU_REGIONS_HPP__

#include "qt_helpers.h"

#include <QAbstractListModel>

class QString;
class QVariant;
class QModelIndex;

//
// Class IARURegions - Qt model that implements the list of IARU regions
//
//
// Responsibilities
//
// 	Provides  a  single column  list  model  that contains  the  human
// 	readable  string  version  of  the  data  region  in  the  display
// 	role. Also  provided is  a translatable  column header  string and
// 	tool tip string.
//
//
// Collaborations
//
// 	Implements a concrete sub-class of the QAbstractListModel class.
//
class IARURegions final : public QAbstractListModel {
    Q_OBJECT

  public:
    //
    // This enumeration  contains the  supported regions,  to complement
    // this an  array of  human readable  strings in  the implementation
    // (IARURegions.cpp) must be maintained in parallel.
    //
    enum Region {
        ALL, // matches with all regions
        R1,
        R2,
        R3,
        SENTINAL // this must be last
    };
    Q_ENUM(Region)

    explicit IARURegions(QObject *parent = nullptr);

    // translate between enumeration and human readable strings
    static char const *name(Region);
    static Region value(QString const &);

    // Implement the QAbstractListModel interface
    int rowCount(QModelIndex const &parent = QModelIndex{}) const override {
        return parent.isValid()
                   ? 0
                   : SENTINAL; // Number of regionss in Region enumeration class
    }
    QVariant data(QModelIndex const &,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation,
                        int = Qt::DisplayRole) const override;
};

// Qt boilerplate  to make the IARURegions::region  enumeration a type
// that can  be streamed and  queued as a  signal argument as  well as
// showing the human readable string when output to debug streams.

#if !defined(QT_NO_DEBUG_STREAM)
ENUM_QDEBUG_OPS_DECL(IARURegions, Region);
#endif

ENUM_QDATASTREAM_OPS_DECL(IARURegions, Region);
ENUM_CONVERSION_OPS_DECL(IARURegions, Region);

#endif
