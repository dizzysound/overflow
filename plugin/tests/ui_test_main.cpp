// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <QApplication>

int main(int argc, char **argv)
{
	QApplication app(argc, argv); // run with QT_QPA_PLATFORM=offscreen
	doctest::Context context(argc, argv);
	return context.run();
}
