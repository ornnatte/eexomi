#include "TickbaseShift.hpp"
#include "source.hpp"
#include "player.hpp"
#include "weapon.hpp"

C_TickbaseShift TickbaseShiftCtx;

bool C_TickbaseShift::ExploitsEnabled( ) {
   return g_Vars.rage.exploit && ( g_Vars.rage.double_tap_bind.enabled || g_Vars.rage.hide_shots_bind.enabled );
}

bool C_TickbaseShift::IsTickcountValid( int tick ) {
   return Source::m_pGlobalVars->tickcount + 64 + this->future_ticks > tick;
}

bool C_TickbaseShift::CanShiftTickbase( void ) {
   auto local = C_CSPlayer::GetLocalPlayer( );
   if ( !local )
	  return true;

   if ( Source::m_pGlobalVars->realtime - g_Vars.globals.m_flLastShotTime < 0.5f && ExploitsEnabled( ) ) // recharge time
	   return true;

   this->ticks_allowed++;

   if ( g_Vars.globals.m_iServerType ) {
	  if ( g_Vars.globals.m_iServerType < 8 )
		 this->max_choke = this->lag_limit = 6;
	  else
		 this->max_choke = this->lag_limit = 16;
   }

   g_Vars.fakelag.lag_limit = this->lag_limit;

   auto disabled_by_server = false;
   auto binds_activated = g_Vars.rage.hide_shots_bind.enabled || g_Vars.rage.double_tap_bind.enabled;
   if ( g_Vars.globals.m_iServerType < 8 ) {
	  disabled_by_server = true;
   } else if ( g_Vars.globals.Fakeducking || ( local->m_fFlags( ) & FL_ONGROUND && g_Vars.misc.fakeduck && g_Vars.misc.fakeduck_bind.enabled ) ) {
	  disabled_by_server = true;
   }

   this->exploits_enabled = g_Vars.rage.exploit && binds_activated && !disabled_by_server;
   return !this->exploits_enabled || this->ticks_allowed > this->max_tickbase_shift;
}

void C_TickbaseShift::SelectShift( void ) {
   auto local = C_CSPlayer::GetLocalPlayer( );
   if ( !local )
	  return;

   this->will_shift_tickbase = this->hold_tickbase_shift ? this->previous_tickbase_shift : 0;

   auto prev_dt_state = this->double_tapped;
   this->double_tapped = false;

   auto weapon = ( C_WeaponCSBaseGun* ) local->m_hActiveWeapon( ).Get( );

   if ( !weapon ) {
	  return;
   }

   auto data = weapon->GetCSWeaponData( );

   if ( this->exploits_enabled ) {
	  auto hide_shots_enabled = g_Vars.rage.hide_shots_bind.enabled;
	  auto double_tap_enabled = g_Vars.rage.double_tap_bind.enabled;

	  this->lag_limit = 1;

	  auto idx = weapon->m_iItemDefinitionIndex( );
	  if ( data->m_iWeaponType != WEAPONTYPE_KNIFE && data->m_iWeaponType != WEAPONTYPE_GRENADE
		   && idx != WEAPON_REVOLVER && idx != WEAPON_ZEUS /*&& !this->hold_tickbase_shift*/ ) {
		 if ( double_tap_enabled ) {
			auto rage_double_tap_fast_recovery = true;
			auto can_shifted_shoot = local->CanShoot( 13 );
			auto can_shoot = local->CanShoot( this->will_shift_tickbase );
			if ( can_shifted_shoot || !can_shoot && this->didnt_shift_tifckbase_previously ) {
			   this->will_shift_tickbase = /*g_Vars.rage.double_tap_type == 1 ? 0 :*/ 14;
			   this->double_tapped = false;
			} else {
			   this->will_shift_tickbase = 0;
			   this->double_tapped = prev_dt_state;
			}
		 } else if ( hide_shots_enabled ) {
			this->will_shift_tickbase = 9;
		 }
	  }
   }
}

void C_TickbaseShift::AdjustPlayerTimeBaseFix( int simulation_ticks ) {
   if ( simulation_ticks < 0 )
	  return;

   auto local = C_CSPlayer::GetLocalPlayer( );
   if ( !local )
	  return;

   // Start in the past so that we get to the sv.time that we'll hit at the end of the
   // frame, just as we process the final command

   if ( Source::m_pGlobalVars->maxClients == 1 ) {
	  // set TickBase so that player simulation tick matches gpGlobals->tickcount after
	  // all commands have been executed
	  local->m_nTickBase( ) = Source::m_pGlobalVars->tickcount - simulation_ticks + Source::m_pGlobalVars->simTicksThisFrame;
   } else { // multiplayer
	  float flCorrectionSeconds = Math::Clamp<float>( g_Vars.sv_clockcorrection_msecs->GetFloat( ) / 1000.0f, 0.0f, 1.0f );
	  int nCorrectionTicks = TIME_TO_TICKS( flCorrectionSeconds );

	  // Set the target tick flCorrectionSeconds (rounded to ticks) ahead in the future. this way the client can
	  // alternate around this target tick without getting smaller than gpGlobals->tickcount.
	  // After running the commands simulation time should be equal or after current gpGlobals->tickcount, 
	  // otherwise the simulation time drops out of the client side interpolated var history window.
	  int	nIdealFinalTick = Source::m_pGlobalVars->tickcount + nCorrectionTicks;

	  int nEstimatedFinalTick = local->m_nTickBase( ) + simulation_ticks;

	  // If client gets ahead of this, we'll need to correct
	  int	 too_fast_limit = nIdealFinalTick + nCorrectionTicks;
	  // If client falls behind this, we'll also need to correct
	  int	 too_slow_limit = nIdealFinalTick - nCorrectionTicks;

	  // See if we are too fast
	  if ( nEstimatedFinalTick > too_fast_limit ||
		   nEstimatedFinalTick < too_slow_limit ) {
		 int nCorrectedTick = nIdealFinalTick - simulation_ticks + Source::m_pGlobalVars->simTicksThisFrame;

		 local->m_nTickBase( ) = nCorrectedTick;
	  }
   }
}

void C_TickbaseShift::ApplyShift( Encrypted_t<CUserCmd> cmd, bool* bSendPacket ) {
   auto local = C_CSPlayer::GetLocalPlayer( );

   if ( !local )
	  return; 

   if ( *bSendPacket ) {
	   	this->commands_to_shift = 0;
		if ( g_Vars.globals.WasShooting || ( cmd->buttons & IN_ATTACK && local->CanShoot( ) ) ) {
			this->commands_to_shift = this->will_shift_tickbase;
			this->didnt_shift_tifckbase_previously = this->will_shift_tickbase <= 0;
			this->double_tapped = this->will_shift_tickbase >= 14;
		}

	  this->hold_tickbase_shift = 0;
	  auto choked_cmds = Source::m_pClientState->m_nChokedCommands( );
	  if ( choked_cmds >= 0 ) {
		 auto command_number = cmd->command_number - choked_cmds;
		 auto next_command_number = cmd->command_number - choked_cmds;
		 do {
			auto pred_cmd = &Source::m_pInput->m_pCommands[ cmd->command_number - 150 * ( command_number / 150 ) - choked_cmds ];
			if ( !pred_cmd || IsTickcountValid( cmd->tick_count ) ) {
			   auto allowed_ticks = --this->ticks_allowed;
			   if ( allowed_ticks <= 0 )
				  allowed_ticks = 0;
			   this->ticks_allowed = allowed_ticks;
			}
			command_number = next_command_number + 1;
			--choked_cmds;
			++next_command_number;
		 } while ( choked_cmds >= 0 );
	  }
   }

   auto allowed_ticks = this->ticks_allowed;
   if ( allowed_ticks > this->max_choke ) {
	  auto next_ticks = allowed_ticks - 1;
	  this->ticks_allowed = next_ticks;
	  if ( next_ticks > 0 ) {
		 this->over_choke_nr = cmd->command_number;
	  } else {
		 this->ticks_allowed = 0;
	  }

	  auto max_allowed = this->max_choke;
	  if ( this->ticks_allowed > max_allowed )
		 this->ticks_allowed = max_allowed;
   }

}

void C_TickbaseShift::OnNewPacket( int command_nr, int tickbase_from_server, int tickbase_from_record, int tickbase_shift ) {
   if ( this->over_choke_nr && command_nr >= this->over_choke_nr )
	  this->over_choke_nr = 0;

   if ( tickbase_shift <= 0 )
	  return;

   if ( tickbase_from_record - tickbase_shift < tickbase_from_server - 1 ) {
	  if ( tickbase_from_server > tickbase_from_record - tickbase_shift )
		 this->ticks_allowed = 0;
   } else {
	  C_CSPlayer::GetLocalPlayer( )->m_nTickBase( ) = tickbase_from_server;
	  Memory::VCall<void( __thiscall* )( void* )>( Source::m_pPrediction.Xor( ), 5 )( Source::m_pPrediction.Xor( ) );
   }
}