/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#define LOGTAG "main"
#include <imagine/logger/logger.h>
#include <imagine/gfx/GfxSprite.hh>
#include <imagine/gfx/GfxText.hh>
#include <imagine/gfx/Renderer.hh>
#include <imagine/gfx/RendererTask.hh>
#include <imagine/gfx/RendererCommands.hh>
#include <imagine/gfx/Projection.hh>
#include <imagine/base/ApplicationContext.hh>
#include <imagine/base/Application.hh>
#include <imagine/base/Screen.hh>
#include <imagine/base/Window.hh>
#include <imagine/util/format.hh>
#include <imagine/util/variant.hh>
#include "tests.hh"
#include "TestPicker.hh"
#include "cpuUtils.hh"
#include "main.hh"
#include <meta.h>
#include <memory>

namespace FrameRateTest
{

static constexpr unsigned framesToRun = 60*60;

struct WindowData
{
	WindowData(IG::ViewAttachParams attachParams):picker{attachParams} {}

	Gfx::Projection proj{};
	IG::WindowRect testRectWin{};
	Gfx::GCRect testRect{};
	TestPicker picker;
	std::unique_ptr<TestFramework> activeTest{};
};

static WindowData &windowData(const IG::Window &win)
{
	return *win.appData<WindowData>();
}

FrameRateTestApplication::FrameRateTestApplication(IG::ApplicationInitParams initParams,
	IG::ApplicationContext &ctx):
	Application{initParams},
	fontManager{ctx},
	renderer{ctx}
{
	IG::WindowConfig winConf{ .title = ctx.applicationName };

	ctx.makeWindow(winConf,
		[this](IG::ApplicationContext ctx, IG::Window &win)
		{
			renderer.initMainTask(&win);
			viewManager = {renderer};
			Gfx::GlyphTextureSet defaultFace{renderer, fontManager.makeSystem(), win.heightScaledMMInPixels(2.5)};
			defaultFace.precacheAlphaNum(renderer);
			defaultFace.precache(renderer, ":.%()");
			viewManager.setDefaultFace(std::move(defaultFace));
			auto &winData = win.makeAppData<WindowData>(IG::ViewAttachParams{viewManager, win, renderer.task()});
			std::vector<TestDesc> testDesc;
			testDesc.emplace_back(TEST_CLEAR, "Clear");
			IG::WP pixmapSize{256, 256};
			for(auto desc: renderer.textureBufferModes())
			{
				testDesc.emplace_back(TEST_DRAW, fmt::format("Draw RGB565 {}x{} ({})", pixmapSize.x, pixmapSize.y, desc.name),
					pixmapSize, desc.mode);
				testDesc.emplace_back(TEST_WRITE, fmt::format("Write RGB565 {}x{} ({})", pixmapSize.x, pixmapSize.y, desc.name),
					pixmapSize, desc.mode);
			}
			auto &picker = winData.picker;
			picker.setTests(testDesc.data(), testDesc.size());
			setPickerHandlers(win);

			win.setOnSurfaceChange(
				[this](IG::Window &win, IG::Window::SurfaceChange change)
				{
					if(change.resized())
					{
						auto viewport = win.viewport(win.contentBounds());
						renderer.task().setDefaultViewport(win, viewport);
						auto &winData = windowData(win);
						winData.proj = renderer.projection(win, viewport,
							Gfx::Mat4::makePerspectiveFovRH(M_PI/4.0, viewport.realAspectRatio(), 1.0, 100.));
						winData.testRectWin = viewport.relRectBestFit({}, 4./3., C2DO, C2DO);
						winData.testRect = winData.proj.plane().unProjectRect(winData.testRectWin);
						placeElements(win);
					}
					renderer.task().updateDrawableForSurfaceChange(win, change);
				});

			ctx.addOnResume(
				[this, &win](IG::ApplicationContext, bool focused)
				{
					windowData(win).picker.prepareDraw();
					auto &activeTest = windowData(win).activeTest;
					if(activeTest)
					{
						activeTest->prepareDraw(renderer);
					}
					return true;
				});

			ctx.addOnExit(
				[this, &win](IG::ApplicationContext ctx, bool backgrounded)
				{
					if(backgrounded)
					{
						if(windowData(win).activeTest)
						{
							finishTest(win, IG::steadyClockTimestamp());
						}
						viewManager.defaultFace().freeCaches();
					}
					return true;
				});

			win.show();
		});

	#ifdef __ANDROID__
	bool manageCPUFreq = false;
	if(manageCPUFreq)
	{
		cpuFreq.emplace();
		if(!(*cpuFreq))
		{
			cpuFreq.reset();
		}
	}
	#endif
}

void FrameRateTestApplication::setPickerHandlers(IG::Window &win)
{
	win.setOnDraw(
		[&task = renderer.task()](IG::Window &win, IG::Window::DrawParams params)
		{
			return task.draw(win, params, {}, [](IG::Window &win, Gfx::RendererCommands &cmds)
			{
				cmds.clear();
				auto &winData = windowData(win);
				auto &picker = winData.picker;
				cmds.basicEffect().setModelViewProjection(cmds, winData.proj);
				picker.draw(cmds);
				cmds.setClipTest(false);
				cmds.present();
			});
		});
	win.setOnInputEvent(
		[this](IG::Window &win, const Input::Event &e)
		{
			if(e.keyEvent() && e.asKeyEvent().pushed(Input::DefaultKey::CANCEL) && !e.asKeyEvent().repeated())
			{
				win.appContext().exit();
				return true;
			}
			return windowData(win).picker.inputEvent(e);
		});
}

void FrameRateTestApplication::setActiveTestHandlers(IG::Window &win)
{
	win.addOnFrame([this, &win](IG::FrameParams params)
		{
			auto atOnFrame = IG::steadyClockTimestamp();
			renderer.setPresentationTime(win, params.presentTime());
			auto &activeTest = windowData(win).activeTest;
			if(activeTest->started)
			{
				activeTest->frameUpdate(renderer.task(), win, params);
			}
			else
			{
				activeTest->started = true;
			}
			activeTest->lastFramePresentTime.timestamp = params.timestamp();
			activeTest->lastFramePresentTime.atOnFrame = atOnFrame;
			if(activeTest->frames == framesToRun || activeTest->shouldEndTest)
			{
				finishTest(win, params.timestamp());
				return false;
			}
			else
			{
				win.setNeedsDraw(true);
				return true;
			}
		});
	win.setOnDraw(
		[this, &task = renderer.task()](IG::Window &win, IG::Window::DrawParams params)
		{
			auto xIndent = viewManager.tableXIndent();
			return task.draw(win, params, {}, [xIndent](IG::Window &win, Gfx::RendererCommands &cmds)
			{
				auto &winData = windowData(win);
				auto &activeTest = winData.activeTest;
				auto rect = winData.testRectWin;
				cmds.basicEffect().setModelViewProjection(cmds, winData.proj);
				activeTest->draw(cmds, cmds.renderer().makeClipRect(win, rect), xIndent);
				activeTest->lastFramePresentTime.atWinPresent = IG::steadyClockTimestamp();
				activeTest->presentFence = cmds.clientWaitSyncReset(activeTest->presentFence);
				cmds.present();
			});
		});
	win.setOnInputEvent(
		[this](IG::Window &win, const Input::Event &e)
		{
			auto &activeTest = windowData(win).activeTest;
			return visit(overloaded
			{
				[&](const Input::MotionEvent &motionEv)
				{
					if(motionEv.pushed() && Config::envIsIOS)
					{
						logMsg("canceled activeTest from pointer input");
						activeTest->shouldEndTest = true;
						return true;
					}
					return false;
				},
				[&](const Input::KeyEvent &keyEv)
				{
					if(keyEv.pushed(Input::DefaultKey::CANCEL))
					{
						logMsg("canceled activeTest from key input");
						activeTest->shouldEndTest = true;
						return true;
					}
					else if(keyEv.pushed(IG::Input::Keycode::D))
					{
						logMsg("posting extra draw");
						win.postDraw();
						return true;
					}
					return false;
				}
			}, e);
		});
}

void FrameRateTestApplication::placeElements(const IG::Window &win)
{
	auto &winData = windowData(win);
	auto &picker = winData.picker;
	auto projP = winData.proj.plane();
	auto &activeTest = winData.activeTest;
	viewManager.setTableXIndentToDefault(win, projP);
	if(!activeTest)
	{
		picker.setViewRect(projP);
		picker.place();
	}
	else
	{
		activeTest->place(renderer, projP, winData.testRect);
	}
}

void FrameRateTestApplication::finishTest(IG::Window &win, IG::FrameTime frameTime)
{
	auto app = win.appContext();
	auto &activeTest = windowData(win).activeTest;
	if(activeTest)
	{
		activeTest->finish(renderer.task(), frameTime);
	}
	renderer.task().awaitPending();
	activeTest.reset();
	deinitCPUFreqStatus();
	deinitCPULoadStatus();
	app.setIdleDisplayPowerSave(true);
	#ifdef __ANDROID__
	if(cpuFreq)
		cpuFreq->setDefaults();
	app.setSustainedPerformanceMode(false);
	#endif
	placeElements(win);
	setPickerHandlers(win);
	win.postDraw();
}

TestFramework *FrameRateTestApplication::startTest(IG::Window &win, const TestParams &t)
{
	auto &face = viewManager.defaultFace();
	auto app = win.appContext();
	#ifdef __ANDROID__
	if(cpuFreq)
		cpuFreq->setLowLatency();
	app.setSustainedPerformanceMode(true);
	#endif
	auto &activeTest = windowData(win).activeTest;
	activeTest = [&] -> std::unique_ptr<TestFramework>
	{
		switch(t.test)
		{
			case TEST_CLEAR: return std::make_unique<ClearTest>();
			case TEST_DRAW: return std::make_unique<DrawTest>();
			case TEST_WRITE: return std::make_unique<WriteTest>();
		}
		bug_unreachable("invalid TestID");
	}();
	activeTest->init(app, renderer, face, t.pixmapSize, t.bufferMode);
	app.setIdleDisplayPowerSave(false);
	initCPUFreqStatus();
	initCPULoadStatus();
	placeElements(win);
	auto &winData = windowData(win);
	setActiveTestHandlers(win);
	return activeTest.get();
}

}

namespace IG
{

const char *const ApplicationContext::applicationName{CONFIG_APP_NAME};

void ApplicationContext::onInit(ApplicationInitParams initParams)
{
	initApplication<FrameRateTest::FrameRateTestApplication>(initParams, *this);
}

}
