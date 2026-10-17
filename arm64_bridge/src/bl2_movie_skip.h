#pragma once

class DynarmicHost;

// Gives Java the full-screen Bink movie state and a skip request
// (bl2::touch::setMovieAccess) once the guest FFullScreenMovieBink code the
// request relies on matches this exact ELF. False leaves the access absent.
bool InstallBl2MovieSkip(DynarmicHost& host);
