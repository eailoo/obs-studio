#include <QAction>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QMenu>
#include <QScreen>
#include "obs-app.hpp"
#include "window-basic-main.hpp"
#include "display-helpers.hpp"
#include "qt-wrappers.hpp"
#include "platform.hpp"

static QList<OBSProjector *> windowedProjectors;
static QList<OBSProjector *> multiviewProjectors;
static bool updatingMultiview = false;
static MultiviewLayout multiviewLayout;

OBSProjector::OBSProjector(QWidget *widget, obs_source_t *source_, int monitor,
		QString title, ProjectorType type_)
	: OBSQTDisplay                 (widget,
	                                Qt::Window),
	  source                       (source_),
	  removedSignal                (obs_source_get_signal_handler(source),
	                                "remove", OBSSourceRemoved, this)
{
	projectorTitle = std::move(title);
	savedMonitor   = monitor;
	isWindow       = savedMonitor < 0;
	type           = type_;

	if (isWindow) {
		setWindowIcon(QIcon(":/res/images/obs.png"));

		UpdateProjectorTitle(projectorTitle);
		windowedProjectors.push_back(this);

		resize(480, 270);
	} else {
		setWindowFlags(Qt::FramelessWindowHint |
				Qt::X11BypassWindowManagerHint);

		QScreen *screen = QGuiApplication::screens()[savedMonitor];
		setGeometry(screen->geometry());

		QAction *action = new QAction(this);
		action->setShortcut(Qt::Key_Escape);
		addAction(action);
		connect(action, SIGNAL(triggered()), this,
				SLOT(EscapeTriggered()));
	}

	setAttribute(Qt::WA_DeleteOnClose, true);

	//disable application quit when last window closed
	setAttribute(Qt::WA_QuitOnClose, false);

	installEventFilter(CreateShortcutFilter());

	auto addDrawCallback = [this] ()
	{
		bool isMultiview = type == ProjectorType::Multiview;
		obs_display_add_draw_callback(GetDisplay(),
				isMultiview ? OBSRenderMultiview : OBSRender,
				this);
		obs_display_set_background_color(GetDisplay(), 0x000000);
	};

	connect(this, &OBSQTDisplay::DisplayCreated, addDrawCallback);

	bool alwaysOnTop = config_get_bool(GetGlobalConfig(), "BasicWindow",
			"ProjectorAlwaysOnTop");
	if (alwaysOnTop && !isWindow)
		SetAlwaysOnTop(this, true);

	bool hideCursor = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "HideProjectorCursor");
	if (hideCursor && !isWindow) {
		QPixmap empty(16, 16);
		empty.fill(Qt::transparent);
		setCursor(QCursor(empty));
	}

	if (type == ProjectorType::Multiview) {
		obs_enter_graphics();

		// All essential action should be placed inside this area
		gs_render_start(true);
		gs_vertex2f(actionSafePercentage, actionSafePercentage);
		gs_vertex2f(actionSafePercentage, 1 - actionSafePercentage);
		gs_vertex2f(1 - actionSafePercentage, 1 - actionSafePercentage);
		gs_vertex2f(1 - actionSafePercentage, actionSafePercentage);
		gs_vertex2f(actionSafePercentage, actionSafePercentage);
		actionSafeMargin = gs_render_save();

		// All graphics should be placed inside this area
		gs_render_start(true);
		gs_vertex2f(graphicsSafePercentage, graphicsSafePercentage);
		gs_vertex2f(graphicsSafePercentage, 1 - graphicsSafePercentage);
		gs_vertex2f(1 - graphicsSafePercentage,
				1 - graphicsSafePercentage);
		gs_vertex2f(1 - graphicsSafePercentage, graphicsSafePercentage);
		gs_vertex2f(graphicsSafePercentage, graphicsSafePercentage);
		graphicsSafeMargin = gs_render_save();

		// 4:3 safe area for widescreen
		gs_render_start(true);
		gs_vertex2f(fourByThreeSafePercentage, graphicsSafePercentage);
		gs_vertex2f(1 - fourByThreeSafePercentage,
				graphicsSafePercentage);
		gs_vertex2f(1 - fourByThreeSafePercentage, 1 -
				graphicsSafePercentage);
		gs_vertex2f(fourByThreeSafePercentage,
				1 - graphicsSafePercentage);
		gs_vertex2f(fourByThreeSafePercentage, graphicsSafePercentage);
		fourByThreeSafeMargin = gs_render_save();

		gs_render_start(true);
		gs_vertex2f(0.0f, 0.5f);
		gs_vertex2f(lineLength, 0.5f);
		leftLine = gs_render_save();

		gs_render_start(true);
		gs_vertex2f(0.5f, 0.0f);
		gs_vertex2f(0.5f, lineLength);
		topLine = gs_render_save();

		gs_render_start(true);
		gs_vertex2f(1.0f, 0.5f);
		gs_vertex2f(1 - lineLength, 0.5f);
		rightLine = gs_render_save();
		obs_leave_graphics();

		UpdateMultiview();

		multiviewProjectors.push_back(this);
	}

	App()->IncrementSleepInhibition();

	if (source)
		obs_source_inc_showing(source);

	ready = true;

	show();

	// We need it here to allow keyboard input in X11 to listen to Escape
	if (!isWindow)
		activateWindow();
}

OBSProjector::~OBSProjector()
{
	bool isMultiview = type == ProjectorType::Multiview;
	obs_display_remove_draw_callback(GetDisplay(),
			isMultiview ? OBSRenderMultiview : OBSRender, this);

	if (source)
		obs_source_dec_showing(source);

	if (isMultiview) {
		for (OBSWeakSource &weakSrc : multiviewScenes) {
			OBSSource src = OBSGetStrongRef(weakSrc);
			if (src)
				obs_source_dec_showing(src);
		}

		obs_enter_graphics();
		gs_vertexbuffer_destroy(actionSafeMargin);
		gs_vertexbuffer_destroy(graphicsSafeMargin);
		gs_vertexbuffer_destroy(fourByThreeSafeMargin);
		gs_vertexbuffer_destroy(leftLine);
		gs_vertexbuffer_destroy(topLine);
		gs_vertexbuffer_destroy(rightLine);
		obs_leave_graphics();
	}

	if (type == ProjectorType::Multiview)
		multiviewProjectors.removeAll(this);

	if (isWindow)
		windowedProjectors.removeAll(this);

	App()->DecrementSleepInhibition();
}

static OBSSource CreateLabel(const char *name, size_t h)
{
	obs_data_t *settings = obs_data_create();
	obs_data_t *font     = obs_data_create();

	std::string text;
	text += " ";
	text += name;
	text += " ";

#if defined(_WIN32)
	obs_data_set_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_string(font, "face", "Helvetica");
#else
	obs_data_set_string(font, "face", "Monospace");
#endif
	obs_data_set_int(font, "flags", 1); // Bold text
	obs_data_set_int(font, "size", int(h / 9.81));

	obs_data_set_obj(settings, "font", font);
	obs_data_set_string(settings, "text", text.c_str());
	obs_data_set_bool(settings, "outline", false);

#ifdef _WIN32
	const char *text_source_id = "text_gdiplus";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	OBSSource txtSource = obs_source_create_private(text_source_id, name,
			settings);
	obs_source_release(txtSource);

	obs_data_release(font);
	obs_data_release(settings);

	return txtSource;
}

static inline void renderVB(gs_effect_t *effect, gs_vertbuffer_t *vb,
		int cx, int cy)
{
	if (!vb)
		return;

	matrix4 transform;
	matrix4_identity(&transform);
	transform.x.x = cx;
	transform.y.y = cy;

	gs_load_vertexbuffer(vb);

	gs_matrix_push();
	gs_matrix_mul(&transform);

	while (gs_effect_loop(effect, "Solid"))
		gs_draw(GS_LINESTRIP, 0, 0);

	gs_matrix_pop();
}

static inline uint32_t labelOffset(obs_source_t *label, uint32_t cx)
{
	uint32_t w = obs_source_get_width(label);

	int n; // Number of scenes per row
	switch (multiviewLayout) {
	default:
		n = 4;
		break;
	}

	w = uint32_t(w * ((1.0f) / n));
	return (cx / 2) - w;
}

static inline void startRegion(int vX, int vY, int vCX, int vCY, float oL,
		float oR, float oT, float oB)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vX, vY, vCX, vCY);
	gs_ortho(oL, oR, oT, oB, -100.0f, 100.0f);
}

static inline void endRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}

void OBSProjector::OBSRenderMultiview(void *data, uint32_t cx, uint32_t cy)
{
	OBSProjector *window = (OBSProjector *)data;

	if (updatingMultiview || !window->ready)
		return;

	OBSBasic     *main   = (OBSBasic *)obs_frontend_get_main_window();
	uint32_t     thickness = 4;
	uint32_t     targetCX, targetCY, offset, thicknessx2 = thickness * 2;
	int          x, y;
	float        fX, fY, halfCX, halfCY, sourceX, sourceY, labelX, labelY,
		     quarterCX, quarterCY, scale, targetCXF, targetCYF,
		     hiCX, hiCY, qiX, qiY, qiCX, qiCY, hiScaleX, hiScaleY,
		     qiScaleX, qiScaleY;

	gs_effect_t  *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t  *color = gs_effect_get_param_by_name(solid, "color");

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	targetCX = ovi.base_width;
	targetCY = ovi.base_height;

	GetScaleAndCenterPos(targetCX, targetCY, cx, cy, x, y, scale);

	targetCXF = float(targetCX);
	targetCYF = float(targetCY);
	fX        = float(x);
	fY        = float(y);

	halfCX    = targetCXF / 2;
	halfCY    = targetCYF / 2;
	hiCX      = halfCX - thicknessx2;
	hiCY      = halfCY - thicknessx2;
	hiScaleX  = (halfCX - thicknessx2) / targetCXF;
	hiScaleY  = (halfCY - thicknessx2) / targetCYF;

	quarterCX = halfCX / 2;
	quarterCY = halfCY / 2;
	qiCX      = quarterCX - thicknessx2;
	qiCY      = quarterCY - thicknessx2;
	qiScaleX  = (quarterCX - thicknessx2) / targetCXF;
	qiScaleY  = (quarterCY - thicknessx2) / targetCYF;

	OBSSource previewSrc = main->GetCurrentSceneSource();
	OBSSource programSrc = main->GetProgramSource();

	bool studioMode = main->IsPreviewProgramMode();

	auto renderVB = [solid, color](gs_vertbuffer_t *vb, int cx, int cy,
			uint32_t colorVal)
	{
		if (!vb)
			return;

		matrix4 transform;
		matrix4_identity(&transform);
		transform.x.x = cx;
		transform.y.y = cy;

		gs_load_vertexbuffer(vb);

		gs_matrix_push();
		gs_matrix_mul(&transform);

		gs_effect_set_color(color, colorVal);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw(GS_LINESTRIP, 0, 0);

		gs_matrix_pop();
	};

	auto drawBox = [solid, color](float cx, float cy, uint32_t colorVal)
	{
		gs_effect_set_color(color, colorVal);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, (uint32_t)cx, (uint32_t)cy);
	};

	auto setRegion = [fX, fY, scale] (float x, float y, float cx, float cy)
	{
		float vX  = int(fX + x * scale);
		float vY  = int(fY + y * scale);
		float vCX = int(cx * scale);
		float vCY = int(cy * scale);

		float oL = x;
		float oT = y;
		float oR = (x + cx);
		float oB = (y + cy);

		startRegion(vX, vY, vCX, vCY, oL, oR, oT, oB);
	};

	auto calcBaseSource = [&](size_t i)
	{
		switch (multiviewLayout) {
		case MultiviewLayout::VERTICAL_LEFT_8_SCENES:
			sourceX = halfCX;
			sourceY = (i / 2 ) * quarterCY;
			if (i % 2 != 0)
				sourceX += quarterCX;
			break;
		case MultiviewLayout::VERTICAL_RIGHT_8_SCENES:
			sourceX = 0;
			sourceY = (i / 2 ) * quarterCY;
			if (i % 2 != 0)
				sourceX = quarterCX;
			break;
		case MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES:
			if (i < 4) {
				sourceX = (float(i) * quarterCX);
				sourceY = 0;
			} else {
				sourceX = (float(i - 4) * quarterCX);
				sourceY = quarterCY;
			}
			break;
		default: // MultiviewLayout::HORIZONTAL_TOP_8_SCENES:
			if (i < 4) {
				sourceX = (float(i) * quarterCX);
				sourceY = halfCY;
			} else {
				sourceX = (float(i - 4) * quarterCX);
				sourceY = halfCY + quarterCY;
			}
		}
		qiX = sourceX + thickness;
		qiY = sourceY + thickness;
	};

	auto calcPreviewProgram = [&](bool program)
	{
		switch (multiviewLayout) {
		case MultiviewLayout::VERTICAL_LEFT_8_SCENES:
			sourceX = thickness;
			sourceY = halfCY + thickness;
			labelX = offset;
			labelY = halfCY * 1.85f;
			if (program) {
				sourceY = thickness;
				labelY = halfCY * 0.85f;
			}
			break;
		case MultiviewLayout::VERTICAL_RIGHT_8_SCENES:
			sourceX = halfCX + thickness;
			sourceY = halfCY + thickness;
			labelX = halfCX + offset;
			labelY = halfCY * 1.85f;
			if (program) {
				sourceY = thickness;
				labelY = halfCY * 0.85f;
			}
			break;
		case MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES:
			sourceX = thickness;
			sourceY = halfCY + thickness;
			labelX = offset;
			labelY = halfCY * 1.85f;
			if (program) {
				sourceX += halfCX;
				labelX += halfCX;
			}
			break;
		default: // MultiviewLayout::HORIZONTAL_TOP_8_SCENES:
			sourceX = thickness;
			sourceY = thickness;
			labelX = offset;
			labelY = halfCY * 0.85f;
			if (program) {
				sourceX += halfCX;
				labelX += halfCX;
			}
		}
	};

	auto paintAreaWithColor = [&](float tx, float ty, float cx, float cy,
			uint32_t color)
	{
		gs_matrix_push();
		gs_matrix_translate3f(tx, ty, 0.0f);
		drawBox(cx, cy, color);
		gs_matrix_pop();
	};

	// Define the whole usable region for the multiview
	startRegion(x, y, targetCX * scale, targetCY * scale, 0.0f, targetCXF,
			0.0f, targetCYF);

	// Change the background color to highlight all sources
	drawBox(targetCXF, targetCYF, outerColor);

	/* ----------------------------- */
	/* draw sources                  */

	for (size_t i = 0; i < 8; i++) {
		OBSSource src = OBSGetStrongRef(window->multiviewScenes[i]);
		obs_source *label = window->multiviewLabels[i + 2];

		// Handle all the offsets
		calcBaseSource(i);

		if (src) {
			// Chose the proper highlight color
			uint32_t colorVal = outerColor;
			if (src == programSrc)
				colorVal = programColor;
			else if (src == previewSrc)
				colorVal = studioMode ? previewColor
						: programColor;

			// Paint the background
			paintAreaWithColor(sourceX, sourceY, quarterCX,
					quarterCY, colorVal);
			paintAreaWithColor(qiX, qiY, qiCX, qiCY,
					backgroundColor);

			/* ----------- */

			// Render the source
			gs_matrix_push();
			gs_matrix_translate3f(qiX, qiY, 0.0f);
			gs_matrix_scale3f(qiScaleX, qiScaleY, 1.0f);
			setRegion(qiX, qiY, qiCX, qiCY);
			obs_source_video_render(src);
			endRegion();
			gs_matrix_pop();

			/* ----------- */

			// Render the label
			if (!label || !config_get_bool(GetGlobalConfig(),
					"BasicWindow", "MultiviewDrawNames"))
				continue;

			offset = labelOffset(label, quarterCX);

			gs_matrix_push();
			gs_matrix_translate3f(sourceX + offset,
					(quarterCY * 0.85f) + sourceY, 0.0f);
			gs_matrix_scale3f(hiScaleX, hiScaleY, 1.0f);
			drawBox(obs_source_get_width(label),
					obs_source_get_height(label) +
					int(quarterCX * 0.015f), labelColor);
			obs_source_video_render(label);
			gs_matrix_pop();
		} else {
			// Paint the background
			paintAreaWithColor(sourceX, sourceY, quarterCX,
					quarterCY, outerColor);
			paintAreaWithColor(qiX, qiY, qiCX, qiCY,
					backgroundColor);
		}
	}

	/* ----------------------------- */
	/* draw preview                  */

	obs_source_t *previewLabel = window->multiviewLabels[0];
	offset = labelOffset(previewLabel, halfCX);
	calcPreviewProgram(false);

	// Paint the background
	paintAreaWithColor(sourceX, sourceY, hiCX, hiCY, backgroundColor);

	// Scale and Draw the preview
	gs_matrix_push();
	gs_matrix_translate3f(sourceX, sourceY, 0.0f);
	gs_matrix_scale3f(hiScaleX, hiScaleY, 1.0f);
	setRegion(sourceX, sourceY, hiCX, hiCY);
	if (studioMode)
		obs_source_video_render(previewSrc);
	else
		obs_render_main_texture();
	if (config_get_bool(GetGlobalConfig(), "BasicWindow",
			"MultiviewDrawAreas")) {
		renderVB(window->actionSafeMargin, targetCX, targetCY,
				outerColor);
		renderVB(window->graphicsSafeMargin, targetCX, targetCY,
				outerColor);
		renderVB(window->fourByThreeSafeMargin, targetCX, targetCY,
				outerColor);
		renderVB(window->leftLine, targetCX, targetCY, outerColor);
		renderVB(window->topLine, targetCX, targetCY, outerColor);
		renderVB(window->rightLine, targetCX, targetCY, outerColor);
	}
	endRegion();
	gs_matrix_pop();

	/* ----------- */

	// Draw the Label
	if (config_get_bool(GetGlobalConfig(), "BasicWindow",
			"MultiviewDrawNames")) {
		gs_matrix_push();
		gs_matrix_translate3f(labelX, labelY, 0.0f);
		gs_matrix_scale3f(hiScaleX, hiScaleY, 1.0f);
		drawBox(obs_source_get_width(previewLabel),
				obs_source_get_height(previewLabel) +
				int(halfCX * 0.015f), labelColor);
		obs_source_video_render(previewLabel);
		gs_matrix_pop();
	}

	/* ----------------------------- */
	/* draw program                  */

	obs_source_t *programLabel = window->multiviewLabels[1];
	offset = labelOffset(programLabel, halfCX);
	calcPreviewProgram(true);

	// Scale and Draw the program
	gs_matrix_push();
	gs_matrix_translate3f(sourceX, sourceY, 0.0f);
	gs_matrix_scale3f(hiScaleX, hiScaleY, 1.0f);
	setRegion(sourceX, sourceY, hiCX, hiCY);
	obs_render_main_texture();
	endRegion();
	gs_matrix_pop();

	/* ----------- */

	// Draw the Label
	if (config_get_bool(GetGlobalConfig(), "BasicWindow",
			"MultiviewDrawNames")) {
		gs_matrix_push();
		gs_matrix_translate3f(labelX, labelY, 0.0f);
		gs_matrix_scale3f(hiScaleX, hiScaleY, 1.0f);
		drawBox(obs_source_get_width(programLabel),
				obs_source_get_height(programLabel) +
				int(halfCX * 0.015f), labelColor);
		obs_source_video_render(programLabel);
		gs_matrix_pop();
	}

	endRegion();
}

void OBSProjector::OBSRender(void *data, uint32_t cx, uint32_t cy)
{
	OBSProjector *window = reinterpret_cast<OBSProjector*>(data);

	if (!window->ready)
		return;

	OBSBasic *main = reinterpret_cast<OBSBasic*>(App()->GetMainWindow());
	OBSSource source = window->source;

	uint32_t targetCX;
	uint32_t targetCY;
	int      x, y;
	int      newCX, newCY;
	float    scale;

	if (source) {
		targetCX = std::max(obs_source_get_width(source), 1u);
		targetCY = std::max(obs_source_get_height(source), 1u);
	} else {
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		targetCX = ovi.base_width;
		targetCY = ovi.base_height;
	}

	GetScaleAndCenterPos(targetCX, targetCY, cx, cy, x, y, scale);

	newCX = int(scale * float(targetCX));
	newCY = int(scale * float(targetCY));

	startRegion(x, y, newCX, newCY, 0.0f, float(targetCX), 0.0f,
			float(targetCY));

	if (window->type == ProjectorType::Preview &&
	    main->IsPreviewProgramMode()) {
		OBSSource curSource = main->GetCurrentSceneSource();

		if (source != curSource) {
			obs_source_dec_showing(source);
			obs_source_inc_showing(curSource);
			source = curSource;
		}
	}

	if (source)
		obs_source_video_render(source);
	else
		obs_render_main_texture();

	endRegion();
}

void OBSProjector::OBSSourceRemoved(void *data, calldata_t *params)
{
	OBSProjector *window = reinterpret_cast<OBSProjector*>(data);

	window->deleteLater();

	UNUSED_PARAMETER(params);
}

static int getSourceByPosition(int x, int y)
{
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	float ratio = float(ovi.base_width) / float(ovi.base_height);

	QWidget *rec  = QApplication::activeWindow();
	int     cx    = rec->width();
	int     cy    = rec->height();
	int     minX  = 0;
	int     minY  = 0;
	int     maxX  = cx;
	int     maxY  = cy;
	int     halfX = cx / 2;
	int     halfY = cy / 2;
	int     pos   = -1;

	switch (multiviewLayout) {
	case MultiviewLayout::VERTICAL_LEFT_8_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			maxX = halfX + (validX / 2);
		} else {
			int validY = cx / ratio;
			minY = halfY - (validY / 2);
			maxY = halfY + (validY / 2);
		}

		minX = halfX;

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = 2 * ((y - minY) / ((maxY - minY) / 4));
		if (x > minX + ((maxX - minX) / 2))
			pos++;
		break;
	case MultiviewLayout::VERTICAL_RIGHT_8_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = halfX - (validX / 2);
		} else {
			int validY = cx / ratio;
			minY = halfY - (validY / 2);
			maxY = halfY + (validY / 2);
		}

		maxX = halfX;

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = 2 * ((y - minY) / ((maxY - minY) / 4));
		if (x > minX + ((maxX - minX) / 2))
			pos++;
		break;
	case MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = halfX - (validX / 2);
			maxX = halfX + (validX / 2);
		} else {
			int validY = cx / ratio;
			minY = halfY - (validY / 2);
		}

		maxY = halfY;

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 4);
		if (y > minY + ((maxY - minY) / 2))
			pos += 4;
		break;
	default: // MultiviewLayout::HORIZONTAL_TOP_8_SCENES
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = halfX - (validX / 2);
			maxX = halfX + (validX / 2);
		} else {
			int validY = cx / ratio;
			maxY = halfY + (validY / 2);
		}

		minY = halfY;

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 4);
		if (y > minY + ((maxY - minY) / 2))
			pos += 4;
	}

	return pos;
}

void OBSProjector::mouseDoubleClickEvent(QMouseEvent *event)
{
	OBSQTDisplay::mouseDoubleClickEvent(event);

	if (!config_get_bool(GetGlobalConfig(), "BasicWindow",
			"MultiviewMouseSwitch"))
		return;

	if (!config_get_bool(GetGlobalConfig(), "BasicWindow",
			"TransitionOnDoubleClick"))
		return;

	OBSBasic *main = (OBSBasic*)obs_frontend_get_main_window();
	if (!main->IsPreviewProgramMode())
		return;

	if (event->button() == Qt::LeftButton) {
		int pos = getSourceByPosition(event->x(), event->y());
		if (pos < 0)
			return;
		OBSSource src = OBSGetStrongRef(multiviewScenes[pos]);
		if (!src)
			return;

		if (main->GetProgramSource() != src)
			main->TransitionToScene(src);
	}
}

void OBSProjector::mousePressEvent(QMouseEvent *event)
{
	OBSQTDisplay::mousePressEvent(event);

	if (event->button() == Qt::RightButton) {
		QMenu popup(this);
		popup.addAction(QTStr("Close"), this, SLOT(EscapeTriggered()));
		popup.exec(QCursor::pos());
	}

	if (event->button() == Qt::LeftButton) {
		if (!config_get_bool(GetGlobalConfig(), "BasicWindow",
				"MultiviewMouseSwitch"))
			return;

		int pos = getSourceByPosition(event->x(), event->y());
		if (pos < 0)
			return;
		OBSSource src = OBSGetStrongRef(multiviewScenes[pos]);
		if (!src)
			return;

		OBSBasic *main = (OBSBasic*)obs_frontend_get_main_window();
		if (main->GetCurrentSceneSource() != src)
			main->SetCurrentScene(src, false);
	}
}

void OBSProjector::EscapeTriggered()
{
	deleteLater();
}

void OBSProjector::UpdateMultiview()
{
	for (OBSWeakSource &val : multiviewScenes)
		val = nullptr;
	for (OBSSource &val : multiviewLabels)
		val = nullptr;

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	uint32_t h = ovi.base_height;

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	int curIdx = 0;

	multiviewLabels[0] = CreateLabel(Str("StudioMode.Preview"), h / 2);
	multiviewLabels[1] = CreateLabel(Str("StudioMode.Program"), h / 2);

	for (size_t i = 0; i < scenes.sources.num && curIdx < 8; i++) {
		obs_source_t *src = scenes.sources.array[i];
		OBSData data = obs_source_get_private_settings(src);
		obs_data_release(data);

		obs_data_set_default_bool(data, "show_in_multiview", true);
		if (!obs_data_get_bool(data, "show_in_multiview"))
			continue;

		multiviewScenes[curIdx] = OBSGetWeakRef(src);
		obs_source_inc_showing(src);

		std::string name;
		name += std::to_string(curIdx + 1);
		name += " - ";
		name += obs_source_get_name(src);

		multiviewLabels[curIdx + 2] = CreateLabel(name.c_str(), h / 3);

		curIdx++;
	}

	obs_frontend_source_list_free(&scenes);

	multiviewLayout = static_cast<MultiviewLayout>(config_get_int(
			GetGlobalConfig(), "BasicWindow", "MultiviewLayout"));
}

void OBSProjector::UpdateProjectorTitle(QString name)
{
	projectorTitle = name;

	QString title = nullptr;
	switch (type) {
	case ProjectorType::Scene:
		title = QTStr("SceneWindow") + " - " + name;
		break;
	case ProjectorType::Source:
		title = QTStr("SourceWindow") + " - " + name;
		break;
	default:
		title = name;
		break;
	}

	setWindowTitle(title);
}

OBSSource OBSProjector::GetSource()
{
	return source;
}

ProjectorType OBSProjector::GetProjectorType()
{
	return type;
}

int OBSProjector::GetMonitor()
{
	return savedMonitor;
}

void OBSProjector::UpdateMultiviewProjectors()
{
	obs_enter_graphics();
	updatingMultiview = true;
	obs_leave_graphics();

	for (auto &projector : multiviewProjectors)
		projector->UpdateMultiview();

	obs_enter_graphics();
	updatingMultiview = false;
	obs_leave_graphics();
}

void OBSProjector::RenameProjector(QString oldName, QString newName)
{
	for (auto &projector : windowedProjectors)
		if (projector->projectorTitle == oldName)
			projector->UpdateProjectorTitle(newName);
}
