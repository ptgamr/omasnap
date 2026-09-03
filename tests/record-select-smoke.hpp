/** @fileoverview Declares the record-target selector smoke test. */
#pragma once

#include <QString>

class QApplication;

/** Checks that picking a recording target produces a rectangle and nothing
 *  else: no image, no shelved document, no operation log. */
[[nodiscard]] bool runRecordSelectSmoke(QApplication &application,
                                        QString &error);
