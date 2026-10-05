@echo off
setlocal
set ROOT=%~dp0..\..
set BUILD=%ROOT%build-team-efficiency

cmake -S "%ROOT%" -B "%BUILD%" -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="%~dp0Inject.cmake" || exit /b 1
cmake --build "%BUILD%" --config Release --target oe_team_efficiency_tests || exit /b 1
ctest --test-dir "%BUILD%" -C Release -R oe_team_efficiency_tests --output-on-failure || exit /b 1
