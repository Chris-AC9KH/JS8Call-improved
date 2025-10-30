#include "revision_utils.hpp"
#include <QCoreApplication>

QString version()
{
#if defined (CMAKE_BUILD)
  QString v {WSJTX_STRINGIZE (WSJTX_VERSION_MAJOR) "." WSJTX_STRINGIZE (WSJTX_VERSION_MINOR) "." WSJTX_STRINGIZE (WSJTX_VERSION_PATCH)};
#if 0
# if defined (WSJTX_RC)
    v += "-rc" WSJTX_STRINGIZE (WSJTX_RC)
# endif
#endif
#else
  QString v {"Not for Release"};
#endif

  return v;
}

QString
program_title()
{
  return QString {"%1-improved (v%2) based on %1 de KN4CRD"}
                 .arg(QCoreApplication::applicationName())
                 .arg(QCoreApplication::applicationVersion());
}

QString
program_version()
{
  return QString {"%1 v%2"}
                 .arg(QCoreApplication::applicationName())
                 .arg(QCoreApplication::applicationVersion());
}
