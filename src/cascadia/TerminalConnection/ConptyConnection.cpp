﻿// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ConptyConnection.h"

#include <windows.h>

#include "ConptyConnection.g.cpp"

#include "../../types/inc/Utils.hpp"
#include "../../types/inc/Environment.hpp"
#include "../../types/inc/UTF8OutPipeReader.hpp"
#include "LibraryResources.h"

using namespace ::Microsoft::Console;

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    // Function Description:
    // - creates some basic anonymous pipes and passes them to CreatePseudoConsole
    // Arguments:
    // - size: The size of the conpty to create, in characters.
    // - phInput: Receives the handle to the newly-created anonymous pipe for writing input to the conpty.
    // - phOutput: Receives the handle to the newly-created anonymous pipe for reading the output of the conpty.
    // - phPc: Receives a token value to identify this conpty
    static HRESULT _CreatePseudoConsoleAndPipes(const COORD size, const DWORD dwFlags, HANDLE* phInput, HANDLE* phOutput, HPCON* phPC)
    {
        RETURN_HR_IF(E_INVALIDARG, phPC == nullptr || phInput == nullptr || phOutput == nullptr);

        wil::unique_hfile outPipeOurSide, outPipePseudoConsoleSide;
        wil::unique_hfile inPipeOurSide, inPipePseudoConsoleSide;

        RETURN_IF_WIN32_BOOL_FALSE(CreatePipe(&inPipePseudoConsoleSide, &inPipeOurSide, NULL, 0));
        RETURN_IF_WIN32_BOOL_FALSE(CreatePipe(&outPipeOurSide, &outPipePseudoConsoleSide, NULL, 0));
        RETURN_IF_FAILED(CreatePseudoConsole(size, inPipePseudoConsoleSide.get(), outPipePseudoConsoleSide.get(), dwFlags, phPC));
        *phInput = inPipeOurSide.release();
        *phOutput = outPipeOurSide.release();
        return S_OK;
    }

    // Function Description:
    // - launches the client application attached to the new pseudoconsole
    HRESULT ConptyConnection::_LaunchAttachedClient() noexcept
    {
        STARTUPINFOEX siEx{ 0 };
        siEx.StartupInfo.cb = sizeof(STARTUPINFOEX);
        siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

        size_t size{};
        // This call will return an error (by design); we are ignoring it.
        InitializeProcThreadAttributeList(NULL, 1, 0, (PSIZE_T)&size);
        auto attrList{ std::make_unique<std::byte[]>(size) };
        siEx.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrList.get());
        RETURN_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, (PSIZE_T)&size));

        RETURN_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(siEx.lpAttributeList,
                                                             0,
                                                             PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                                             _hPC.get(),
                                                             sizeof(HPCON),
                                                             NULL,
                                                             NULL));

        std::wstring cmdline{ wil::ExpandEnvironmentStringsW<std::wstring>(_commandline.c_str()) }; // mutable copy -- required for CreateProcessW

        Utils::EnvironmentVariableMapW environment;

        {
            // Convert connection Guid to string and ignore the enclosing '{}'.
            std::wstring wsGuid{ Utils::GuidToString(_guid) };
            wsGuid.pop_back();

            const wchar_t* const pwszGuid{ wsGuid.data() + 1 };

            // Ensure every connection has the unique identifier in the environment.
            environment.emplace(L"WT_SESSION", pwszGuid);
        }

        std::vector<wchar_t> newEnvVars;
        auto zeroNewEnv = wil::scope_exit([&] {
            ::SecureZeroMemory(newEnvVars.data(),
                               newEnvVars.size() * sizeof(decltype(newEnvVars.begin())::value_type));
        });

        auto zeroEnvMap = wil::scope_exit([&] {
            // Can't zero the keys, but at least we can zero the values.
            for (auto& [name, value] : environment)
            {
                ::SecureZeroMemory(value.data(), value.size() * sizeof(decltype(value.begin())::value_type));
            }

            environment.clear();
        });

        RETURN_IF_FAILED(Utils::UpdateEnvironmentMapW(environment));
        RETURN_IF_FAILED(Utils::EnvironmentMapToEnvironmentStringsW(environment, newEnvVars));

        LPWCH lpEnvironment = newEnvVars.empty() ? nullptr : newEnvVars.data();

        // If we have a startingTitle, create a mutable character buffer to add
        // it to the STARTUPINFO.
        std::wstring mutableTitle{};
        if (!_startingTitle.empty())
        {
            mutableTitle = _startingTitle;
            siEx.StartupInfo.lpTitle = mutableTitle.data();
        }

        const wchar_t* const startingDirectory = _startingDirectory.size() > 0 ? _startingDirectory.c_str() : nullptr;

        RETURN_IF_WIN32_BOOL_FALSE(CreateProcessW(
            nullptr,
            cmdline.data(),
            nullptr, // lpProcessAttributes
            nullptr, // lpThreadAttributes
            false, // bInheritHandles
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, // dwCreationFlags
            lpEnvironment, // lpEnvironment
            startingDirectory,
            &siEx.StartupInfo, // lpStartupInfo
            &_piClient // lpProcessInformation
            ));

        DeleteProcThreadAttributeList(siEx.lpAttributeList);

        return S_OK;
    }

    ConptyConnection::ConptyConnection(const hstring& commandline,
                                       const hstring& startingDirectory,
                                       const hstring& startingTitle,
                                       const uint32_t initialRows,
                                       const uint32_t initialCols,
                                       const guid& initialGuid) :
        _initialRows{ initialRows },
        _initialCols{ initialCols },
        _commandline{ commandline },
        _startingDirectory{ startingDirectory },
        _startingTitle{ startingTitle },
        _guid{ initialGuid }
    {
        if (_guid == guid{})
        {
            _guid = Utils::CreateGuid();
        }
    }

    winrt::guid ConptyConnection::Guid() const noexcept
    {
        return _guid;
    }

    void ConptyConnection::Start()
    try
    {
        const COORD dimensions{ gsl::narrow_cast<SHORT>(_initialCols), gsl::narrow_cast<SHORT>(_initialRows) };
        THROW_IF_FAILED(_CreatePseudoConsoleAndPipes(dimensions, 0, &_inPipe, &_outPipe, &_hPC));
        THROW_IF_FAILED(_LaunchAttachedClient());

        _startTime = std::chrono::high_resolution_clock::now();

        // Create our own output handling thread
        // This must be done after the pipes are populated.
        // Each connection needs to make sure to drain the output from its backing host.
        _hOutputThread.reset(CreateThread(
            nullptr,
            0,
            [](LPVOID lpParameter) {
                ConptyConnection* const pInstance = reinterpret_cast<ConptyConnection*>(lpParameter);
                return pInstance->_OutputThread();
            },
            this,
            0,
            nullptr));

        THROW_LAST_ERROR_IF_NULL(_hOutputThread);

        _clientExitWait.reset(CreateThreadpoolWait(
            [](PTP_CALLBACK_INSTANCE /*callbackInstance*/, PVOID context, PTP_WAIT /*wait*/, TP_WAIT_RESULT /*waitResult*/) {
                ConptyConnection* const pInstance = reinterpret_cast<ConptyConnection*>(context);
                pInstance->_ClientTerminated();
            },
            this,
            nullptr));

        SetThreadpoolWait(_clientExitWait.get(), _piClient.hProcess, nullptr);

        _transitionToState(ConnectionState::Connected);
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();

        winrt::hstring failureText{ wil::str_printf<std::wstring>(RS_(L"ProcessFailedToLaunch").c_str(), static_cast<unsigned int>(hr), _commandline.c_str()) };
        _TerminalOutputHandlers(failureText);
        _transitionToState(ConnectionState::Failed);
    }

    void ConptyConnection::_ClientTerminated() noexcept
    {
        if (_state >= ConnectionState::Closing)
        {
            // This termination was expected.
            return;
        }

        DWORD exitCode{ 0 };
        GetExitCodeProcess(_piClient.hProcess, &exitCode);

        if (exitCode == 0)
        {
            _transitionToState(ConnectionState::Closed);
        }
        else
        {
            winrt::hstring failureText{ wil::str_printf<std::wstring>(RS_(L"ProcessExitedUnexpectedly").c_str(), (unsigned int)exitCode) };
            _TerminalOutputHandlers(L"\r\n");
            _TerminalOutputHandlers(failureText);
            _transitionToState(ConnectionState::Failed);
        }

        // We don't need these any longer.
        _piClient.reset();
        _hPC.reset();
    }

    bool ConptyConnection::_transitionToState(const ConnectionState state) noexcept
    {
        {
            std::lock_guard<std::mutex> stateLock{ _stateMutex };
            // only allow movement up the state gradient
            if (state < _state)
            {
                return false;
            }
            _state = state;
        }
        // Dispatch the event outside of lock.
        _StateChangedHandlers(*this, nullptr);
        return true;
    }

    void ConptyConnection::WriteInput(hstring const& data)
    {
        if (_state != ConnectionState::Connected)
        {
            return;
        }

        // convert from UTF-16LE to UTF-8 as ConPty expects UTF-8
        // TODO GH#3378 reconcile and unify UTF-8 converters
        std::string str = winrt::to_string(data);
        LOG_IF_WIN32_BOOL_FALSE(WriteFile(_inPipe.get(), str.c_str(), (DWORD)str.length(), nullptr, nullptr));
    }

    void ConptyConnection::Resize(uint32_t rows, uint32_t columns)
    {
        if (!_hPC)
        {
            _initialRows = rows;
            _initialCols = columns;
        }
        else if (_state == ConnectionState::Connected)
        {
            THROW_IF_FAILED(ResizePseudoConsole(_hPC.get(), { Utils::ClampToShortMax(columns, 1), Utils::ClampToShortMax(rows, 1) }));
        }
    }

    void ConptyConnection::Close()
    {
        if (_transitionToState(ConnectionState::Closing))
        {
            _clientExitWait.reset(); // immediately stop waiting for the client to exit.

            _hPC.reset(); // tear down the pseudoconsole (this is like clicking X on a console window)

            _inPipe.reset(); // break the pipes
            _outPipe.reset();

            if (_hOutputThread)
            {
                // Tear down our output thread -- now that the output pipe was closed on the
                // far side, we can run down our local reader.
                LOG_LAST_ERROR_IF(WAIT_FAILED == WaitForSingleObject(_hOutputThread.get(), INFINITE));
                _hOutputThread.reset();
            }

            if (_piClient.hProcess)
            {
                // Wait for the client to terminate (which it should do successfully)
                LOG_LAST_ERROR_IF(WAIT_FAILED == WaitForSingleObject(_piClient.hProcess, INFINITE));
                _piClient.reset();
            }

            _transitionToState(ConnectionState::Closed);
        }
    }

    DWORD ConptyConnection::_OutputThread()
    {
        UTF8OutPipeReader pipeReader{ _outPipe.get() };
        std::string_view strView{};

        // process the data of the output pipe in a loop
        while (true)
        {
            HRESULT result = pipeReader.Read(strView);
            if (FAILED(result) || result == S_FALSE)
            {
                if (_state >= ConnectionState::Closing)
                {
                    // This termination was expected.
                    return 0;
                }

                _transitionToState(ConnectionState::Failed);
                return (DWORD)-1;
            }

            if (strView.empty())
            {
                return 0;
            }

            if (!_recievedFirstByte)
            {
                auto now = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> delta = now - _startTime;

                TraceLoggingWrite(g_hTerminalConnectionProvider,
                                  "RecievedFirstByte",
                                  TraceLoggingDescription("An event emitted when the connection recieves the first byte"),
                                  TraceLoggingGuid(_guid, "SessionGuid", "The WT_SESSION's GUID"),
                                  TraceLoggingFloat64(delta.count(), "Duration"),
                                  TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                                  TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));
                _recievedFirstByte = true;
            }

            // Convert buffer to hstring
            auto hstr{ winrt::to_hstring(strView) };

            // Pass the output to our registered event handlers
            _TerminalOutputHandlers(hstr);
        }

        return 0;
    }

}
