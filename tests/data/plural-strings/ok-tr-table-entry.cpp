return QStringLiteral("%1 %2").arg(i).arg(GeneratorForm::tr(names[i & 15]));
QString a = tr(entry.text);
QString b = tr(msg.toUtf8().constData(), "menu");
QString c = QCoreApplication::translate(kCtx, m.text, nullptr);
