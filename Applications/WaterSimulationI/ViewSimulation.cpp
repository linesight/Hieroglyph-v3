//--------------------------------------------------------------------------------
// This file is a portion of the Hieroglyph 3 Rendering Engine.  It is distributed
// under the MIT License, available in the root of this distribution and 
// at the following URL:
//
// http://www.opensource.org/licenses/mit-license.php
//
// Copyright (c) Jason Zink 
//--------------------------------------------------------------------------------

//--------------------------------------------------------------------------------
#include "ViewSimulation.h"
#include "Entity3D.h"
#include "Node3D.h"
#include "Texture2dConfigDX11.h"
#include "Log.h"
#include <sstream>
#include "IParameterManager.h"
#include "BufferConfigDX11.h"
//--------------------------------------------------------------------------------
using namespace Glyph3;
//--------------------------------------------------------------------------------
ViewSimulation::ViewSimulation( RendererDX11& Renderer, int SizeX, int SizeY )
{
    // Initialize simulation dimensions
    ThreadGroupsX = SizeX;
    ThreadGroupsY = SizeY;
    const int PointCountX = SizeX * 16;
    const int PointCountY = SizeY * 16;

    // Create initial height data for simulation
    std::vector<GridPoint> initialData(PointCountX * PointCountY);
    for (int j = 0; j < PointCountY; j++) {
        for (int i = 0; i < PointCountX; i++) {
            const int x = i - 32;
            const int y = j - 96;
            const float frequency = 0.1f;

            GridPoint& point = initialData[PointCountX * j + i];

            if (x * x + y * y != 0) {
                float distance = sqrt(static_cast<float>(x * x + y * y));
                point.height = 40.0f * sinf(distance * frequency) / (distance * frequency);
            }
            else {
                point.height = 40.0f;
            }
            point.flow.MakeZero();
        }
    }

    // Create water state buffers
    D3D11_SUBRESOURCE_DATA initialSubresourceData{};
    initialSubresourceData.pSysMem = initialData.data();

    BufferConfigDX11 waterBufferConfig;
    waterBufferConfig.SetDefaultStructuredBuffer(PointCountX * PointCountY, sizeof(GridPoint));

    WaterState[0] = Renderer.CreateStructuredBuffer(&waterBufferConfig, &initialSubresourceData);
    WaterState[1] = Renderer.CreateStructuredBuffer(&waterBufferConfig, &initialSubresourceData);

    // Setup water simulation effect
    pWaterEffect = new RenderEffectDX11();
    pWaterEffect->SetComputeShader(Renderer.LoadShader(
        COMPUTE_SHADER,
        std::wstring(L"WaterSimulation.hlsl"),
        std::wstring(L"CSMAIN"),
        std::wstring(L"cs_4_0")
    ));

    // Get parameter references
    m_pCurrentWaterState = Renderer.m_pParamMgr->GetShaderResourceParameterRef(std::wstring(L"CurrentWaterState"));
    m_pNewWaterState = Renderer.m_pParamMgr->GetUnorderedAccessParameterRef(std::wstring(L"NewWaterState"));
    m_pTimeParameters = Renderer.m_pParamMgr->GetConstantBufferParameterRef(std::wstring(L"TimeParameters"));
    m_pDispatchSize = Renderer.m_pParamMgr->GetVectorParameterRef(std::wstring(L"DispatchSize"));

    // Create time parameters constant buffer
    BufferConfigDX11 timeBufferConfig;
    timeBufferConfig.SetDefaultConstantBuffer(sizeof(Vector4f) * 2, true);
    m_pTimeParametersCB = Renderer.CreateConstantBuffer(&timeBufferConfig, nullptr, true);

    // Validate and set parameters
    bool success = true;

    // Validate parameter references
    if (!m_pCurrentWaterState) {
        Log::Get().Write(L"ERROR: Could not get CurrentWaterState parameter reference!");
        success = false;
    }
    if (!m_pNewWaterState) {
        Log::Get().Write(L"ERROR: Could not get NewWaterState parameter reference!");
        success = false;
    }
    if (!m_pTimeParameters) {
        Log::Get().Write(L"ERROR: Could not get TimeParameters constant buffer reference!");
        success = false;
    }
    if (!m_pDispatchSize) {
        Log::Get().Write(L"ERROR: Could not get DispatchSize parameter reference!");
        success = false;
    }

    // Validate resource creation
    if (!WaterState[0] || WaterState[0]->m_iResourceSRV < 0) {
        Log::Get().Write(L"ERROR: Failed to create valid WaterState[0] buffer!");
        success = false;
    }
    if (!WaterState[1] || WaterState[1]->m_iResourceUAV < 0) {
        Log::Get().Write(L"ERROR: Failed to create valid WaterState[1] buffer!");
        success = false;
    }
    if (!m_pTimeParametersCB || m_pTimeParametersCB->m_iResource < 0) {
        Log::Get().Write(L"ERROR: Failed to create TimeParameters constant buffer!");
        success = false;
    }

    // Set parameters if validation passed
    if (success) {
        // Set water state parameters
        Renderer.m_pParamMgr->SetShaderResourceParameter(m_pCurrentWaterState, WaterState[0]);
        Renderer.m_pParamMgr->SetUnorderedAccessParameter(m_pNewWaterState, WaterState[1]);

        // Set dispatch size
        Vector4f dispatchSize(static_cast<float>(SizeX), static_cast<float>(SizeY),
            static_cast<float>(PointCountX), static_cast<float>(PointCountY));
        Renderer.m_pParamMgr->SetVectorParameter(m_pDispatchSize, &dispatchSize);

        // Set time parameters constant buffer
        Renderer.m_pParamMgr->SetConstantBufferParameter(m_pTimeParameters, m_pTimeParametersCB);

        Log::Get().Write(L"ViewSimulation: Initialization completed successfully");
    }
    else {
        Log::Get().Write(L"ViewSimulation: Initialization completed with errors");
    }
}
//--------------------------------------------------------------------------------
ViewSimulation::~ViewSimulation()
{
	SAFE_DELETE( pWaterEffect );
}
//--------------------------------------------------------------------------------
void ViewSimulation::Update( float fTime )
{
}
//--------------------------------------------------------------------------------
void ViewSimulation::QueuePreTasks( RendererDX11* pRenderer )
{
	// Queue this view into the renderer for processing.  Since this is a 
	// simulation style view, there is no root and hence no additional recursive
	// 'PreDraw'ing required.
	pRenderer->QueueTask( this );
}
//--------------------------------------------------------------------------------
void ViewSimulation::ExecuteTask( PipelineManagerDX11* pPipelineManager, IParameterManager* pParamManager )
{
	// Set this view's render parameters.
	SetRenderParams( pParamManager );

	// Perform the dispatch call to update the simulation state.
	pPipelineManager->Dispatch( *pWaterEffect, ThreadGroupsX, ThreadGroupsY, 1, pParamManager );
	pPipelineManager->ClearPipelineResources();
	pPipelineManager->ApplyPipelineResources();

	// Switch the two resources so that the current state is maintained in slot 0.
	ResourcePtr TempState = WaterState[0];
	WaterState[0] = WaterState[1];
	WaterState[1] = TempState;
}
//--------------------------------------------------------------------------------
void ViewSimulation::SetRenderParams( IParameterManager* pParamManager )
{
	// Set the parameters for this view to be able to perform its processing
	// sequence.  In this case, water state '0' is always considered the current
	// state.

	// Verify SRV and UAV indices are valid before setting parameters
	if (m_pCurrentWaterState && WaterState[0]->m_iResourceSRV >= 0) {
		pParamManager->SetShaderResourceParameter(m_pCurrentWaterState, WaterState[0]);
	}
	else {
		Log::Get().Write(L"ERROR: Cannot set CurrentWaterState - invalid parameter or SRV!");
	}

	if (m_pNewWaterState && WaterState[1]->m_iResourceUAV >= 0) {
		pParamManager->SetUnorderedAccessParameter(m_pNewWaterState, WaterState[1]);
	}
	else {
		Log::Get().Write(L"ERROR: Cannot set NewWaterState - invalid parameter or UAV!");
	}

	// Set time parameters (you might want to update these each frame)
	Vector4f TimeFactors = Vector4f(0.016f, 0.0f, 0.0f, 0.0f); // Example: 60 FPS delta time
	if (m_pDispatchSize) {
		pParamManager->SetVectorParameter(m_pDispatchSize, &TimeFactors);
	}
}
//--------------------------------------------------------------------------------
void ViewSimulation::SetUsageParams( IParameterManager* pParamManager )
{
	// Set the parameters for allowing an application to use the current state
	// as a height map via a shader resource view.

	Vector4f DispatchSize = Vector4f( 16.0f, 16.0f, 16.0f * 16.0f, 16.0f * 16.0f );

	pParamManager->SetShaderResourceParameter( m_pCurrentWaterState, WaterState[0] );
	pParamManager->SetVectorParameter( m_pDispatchSize, &DispatchSize );
}
//--------------------------------------------------------------------------------
std::wstring ViewSimulation::GetName()
{
	return( L"ViewSimulation" );
}
//--------------------------------------------------------------------------------
