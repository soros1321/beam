// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "common.h"
#include "ecc_native.h"

namespace ECC {

	/////////////////////
	// InnerProduct

	thread_local InnerProduct::BatchContext* InnerProduct::BatchContext::s_pInstance = NULL;

	InnerProduct::BatchContext::BatchContext(uint32_t nCasualTotal)
		:m_CasualTotal(nCasualTotal)
		,m_bEnableBatch(false)
	{
		m_ppPrepared = m_Bufs.m_ppPrepared;
		m_pKPrep = m_Bufs.m_pKPrep;
		m_pAuxPrepared = m_Bufs.m_pAuxPrepared;

		for (uint32_t j = 0; j < 2; j++)
			for (uint32_t i = 0; i < InnerProduct::nDim; i++)
				m_ppPrepared[i + j * InnerProduct::nDim] = &Context::get().m_Ipp.m_pGen_[j][i];

		m_ppPrepared[s_Idx_GenDot] = &Context::get().m_Ipp.m_GenDot_;
		m_ppPrepared[s_Idx_Aux2] = &Context::get().m_Ipp.m_Aux2_;
		m_ppPrepared[s_Idx_G] = &Context::get().m_Ipp.G_;
		m_ppPrepared[s_Idx_H] = &Context::get().m_Ipp.H_;

		m_Prepared = s_CountPrepared;
		Reset();
	}

	void InnerProduct::BatchContext::Reset()
	{
		m_Casual = 0;
		ZeroObject(m_Bufs.m_pKPrep);
		m_bDirty = false;
	}

	void InnerProduct::BatchContext::Calculate(Point::Native& res)
	{
		Mode::Scope scope(Mode::Fast);
		MultiMac::Calculate(res);
	}

	bool InnerProduct::BatchContext::AddCasual(const Point& p, const Scalar::Native& k)
	{
		Point::Native pt;
		if (!pt.Import(p))
			return false;

		AddCasual(pt, k);
		return true;
	}

	void InnerProduct::BatchContext::AddCasual(const Point::Native& pt, const Scalar::Native& k)
	{
		assert(uint32_t(m_Casual) < m_CasualTotal);

		Casual& c = m_pCasual[m_Casual++];

		c.Init(pt, k);
		if (m_bEnableBatch)
			c.m_K *= m_Multiplier;
	}

	void InnerProduct::BatchContext::AddPrepared(uint32_t i, const Scalar::Native& k)
	{
		assert(i < s_CountPrepared);
		Scalar::Native& trg = m_Bufs.m_pKPrep[i];

		trg += m_bEnableBatch ? (k * m_Multiplier) : k;
	}

	bool InnerProduct::BatchContext::Flush()
	{
		if (!m_bDirty)
			return true;

		Point::Native pt;
		Calculate(pt);
		if (!(pt == Zero))
			return false;

		Reset();
		return true;
	}

	bool InnerProduct::BatchContext::EquationBegin(uint32_t nCasualNeeded)
	{
		if (nCasualNeeded > m_CasualTotal)
		{
			assert(false);
			return false; // won't fit!
		}

		nCasualNeeded += m_Casual;
		if (nCasualNeeded > m_CasualTotal)
		{
			if (!Flush())
				return false;
		}

		m_bDirty = true;
		return true;
	}

	bool InnerProduct::BatchContext::EquationEnd()
	{
		assert(m_bDirty);

		if (!m_bEnableBatch)
			return Flush();

		return true;
	}


	struct InnerProduct::Calculator
	{
		struct ChallengeSet {
			Scalar::Native m_DotMultiplier;
			Scalar::Native m_Val[nCycles][2];
		};

		struct ModifierExpanded
		{
			Scalar::Native m_pPwr[2][nDim];
			bool m_pUse[2];

			void Init(const Modifier& mod)
			{
				for (size_t j = 0; j < _countof(mod.m_pMultiplier); j++)
				{
					m_pUse[j] = (NULL != mod.m_pMultiplier[j]);
					if (m_pUse[j])
					{
						m_pPwr[j][0] = 1U;
						for (uint32_t i = 1; i < nDim; i++)
							m_pPwr[j][i] = m_pPwr[j][i - 1] * *(mod.m_pMultiplier[j]);
					}
				}
			}

			void Set(Scalar::Native& dst, const Scalar::Native& src, int i, int j) const
			{
				if (m_pUse[j])
					dst = src * m_pPwr[j][i];
				else
					dst = src;
			}
		};

		static void get_Challenge(Scalar::Native* pX, Oracle&);

		struct Aggregator
		{
			MultiMac& m_Mm;
			const ChallengeSet& m_cs;
			const ModifierExpanded& m_Mod;
			const Calculator* m_pCalc; // set if source are already condensed points
			InnerProduct::BatchContext* m_pBatchCtx;
			const int m_j;
			const unsigned int m_iCycleTrg;

			Aggregator(MultiMac& mm, const ChallengeSet& cs, const ModifierExpanded& mod, int j, unsigned int iCycleTrg)
				:m_Mm(mm)
				,m_cs(cs)
				,m_Mod(mod)
				,m_pCalc(NULL)
				,m_pBatchCtx(NULL)
				,m_j(j)
				,m_iCycleTrg(iCycleTrg)				
			{
			}

			void Proceed(uint32_t iPos, uint32_t iCycle, const Scalar::Native& k);
		};

		static const uint32_t s_iCycle0 = 2; // condense source generators into points (after 3 iterations, 8 points)

		Point::Native m_pGen[2][nDim >> (1 + s_iCycle0)];
		Scalar::Native m_pVal[2][nDim >> 1];

		const Scalar::Native* m_ppSrc[2];

		ModifierExpanded m_Mod;
		ChallengeSet m_Cs;

		MultiMac_WithBufs<(nDim >> (s_iCycle0 + 1)), nDim * 2> m_Mm;

		uint32_t m_iCycle;
		uint32_t m_n;
		uint32_t m_GenOrder;

		void Condense();
		void ExtractLR(int j);
	};


	void InnerProduct::Calculator::get_Challenge(Scalar::Native* pX, Oracle& oracle)
	{
		do
			oracle >> pX[0];
		while (pX[0] == Zero);

		pX[1].SetInv(pX[0]);
	}

	void InnerProduct::Calculator::Condense()
	{
		// Vectors
		for (int j = 0; j < 2; j++)
			for (uint32_t i = 0; i < m_n; i++)
			{
				// dst and src need not to be distinct
				m_pVal[j][i] = m_ppSrc[j][i] * m_Cs.m_Val[m_iCycle][j];
				m_pVal[j][i] += m_ppSrc[j][m_n + i] * m_Cs.m_Val[m_iCycle][!j];
			}

		// Points
		switch (m_iCycle)
		{
		case s_iCycle0:
			// further compression points (casual)
			// Currently according to benchmarks - not necessary
			break;

		case nCycles - 1: // last iteration - no need to condense points
		default:
			return;
		}

		for (int j = 0; j < 2; j++)
			for (uint32_t i = 0; i < m_n; i++)
			{
				m_Mm.Reset();

				Point::Native& g0 = m_pGen[j][i];

				Aggregator aggr(m_Mm, m_Cs, m_Mod, j, nCycles - m_iCycle - 1);

				if (m_iCycle > s_iCycle0)
					aggr.m_pCalc = this;

				aggr.Proceed(i, m_GenOrder, 1U);

				m_Mm.Calculate(g0);
			}

		m_GenOrder = nCycles - m_iCycle - 1;
	}

	void InnerProduct::Calculator::ExtractLR(int j)
	{
		m_Mm.Reset();

		// Cross-term
		Scalar::Native& crossTrm = m_Mm.m_pKPrep[m_Mm.m_Prepared];
		m_Mm.m_ppPrepared[m_Mm.m_Prepared++] = &Context::get().m_Ipp.m_GenDot_;

		crossTrm = Zero;

		for (uint32_t i = 0; i < m_n; i++)
			crossTrm += m_ppSrc[j][i] * m_ppSrc[!j][m_n + i];

		crossTrm *= m_Cs.m_DotMultiplier;

		// other
		for (int jSrc = 0; jSrc < 2; jSrc++)
		{
			uint32_t off0 = (jSrc == j) ? 0 : m_n;
			uint32_t off1 = (jSrc == j) ? m_n : 0;

			for (uint32_t i = 0; i < m_n; i++)
			{
				const Scalar::Native& v = m_ppSrc[jSrc][i + off0];

				Aggregator aggr(m_Mm, m_Cs, m_Mod, jSrc, nCycles - m_iCycle);

				if (m_iCycle > s_iCycle0)
					aggr.m_pCalc = this;

				aggr.Proceed(i + off1, m_GenOrder, v);
			}
		}
	}

	void InnerProduct::Calculator::Aggregator::Proceed(uint32_t iPos, uint32_t iCycle, const Scalar::Native& k)
	{
		if (iCycle != m_iCycleTrg)
		{
			assert(iCycle <= nCycles);
			Scalar::Native k0 = k;
			k0 *= m_cs.m_Val[nCycles - iCycle][!m_j];

			Proceed(iPos, iCycle - 1, k0);

			k0 = k;
			k0 *= m_cs.m_Val[nCycles - iCycle][m_j];

			uint32_t nStep = 1 << (iCycle - 1);

			Proceed(iPos + nStep, iCycle - 1, k0);

		} else
		{
			if (m_pCalc)
			{
				assert(iPos < _countof(m_pCalc->m_pGen[m_j]));
				m_Mm.m_pCasual[m_Mm.m_Casual++].Init(m_pCalc->m_pGen[m_j][iPos], k);
			}
			else
			{
				assert(iPos < nDim);

				if (m_pBatchCtx)
				{
					Scalar::Native k2;
					m_Mod.Set(k2, k, iPos, m_j);

					m_pBatchCtx->m_Bufs.m_pKPrep[iPos + m_j * InnerProduct::nDim] += k2;
				}
				else
				{
					m_Mod.Set(m_Mm.m_pKPrep[m_Mm.m_Prepared], k, iPos, m_j);
					m_Mm.m_ppPrepared[m_Mm.m_Prepared++] = &Context::get().m_Ipp.m_pGen_[m_j][iPos];
				}
			}
		}
	}

	void InnerProduct::get_Dot(Scalar::Native& res, const Scalar::Native* pA, const Scalar::Native* pB)
	{
		static_assert(nDim, "");
		res = pA[0];
		res *= pB[0];

		Scalar::Native tmp;

		for (uint32_t i = 1; i < nDim; i++)
		{
			tmp = pA[i];
			tmp *= pB[i];
			res += tmp;
		}
	}

	void InnerProduct::Create(Point::Native& commAB, const Scalar::Native& dotAB, const Scalar::Native* pA, const Scalar::Native* pB, const Modifier& mod)
	{
		Oracle oracle;
		Create(oracle, &commAB, dotAB, pA, pB, mod);
	}

	void InnerProduct::Create(Oracle& oracle, const Scalar::Native& dotAB, const Scalar::Native* pA, const Scalar::Native* pB, const Modifier& mod)
	{
		Create(oracle, NULL, dotAB, pA, pB, mod);
	}

	void InnerProduct::Create(Oracle& oracle, Point::Native* pAB, const Scalar::Native& dotAB, const Scalar::Native* pA, const Scalar::Native* pB, const Modifier& mod)
	{
		Mode::Scope scope(Mode::Fast);

		Calculator c;
		c.m_Mod.Init(mod);
		c.m_GenOrder = nCycles;
		c.m_ppSrc[0] = pA;
		c.m_ppSrc[1] = pB;

		if (pAB)
		{
			for (int j = 0; j < 2; j++)
				for (uint32_t i = 0; i < nDim; i++, c.m_Mm.m_Prepared++)
				{
					c.m_Mm.m_ppPrepared[c.m_Mm.m_Prepared] = &Context::get().m_Ipp.m_pGen_[j][i];
					c.m_Mod.Set(c.m_Mm.m_pKPrep[c.m_Mm.m_Prepared], c.m_ppSrc[j][i], i, j);
				}

			c.m_Mm.Calculate(*pAB);

			oracle << *pAB;
		}

		oracle << dotAB >> c.m_Cs.m_DotMultiplier;

		Point::Native comm;

		for (c.m_iCycle = 0; c.m_iCycle < nCycles; c.m_iCycle++)
		{
			c.m_n = nDim >> (c.m_iCycle + 1);

			Calculator::get_Challenge(c.m_Cs.m_Val[c.m_iCycle], oracle);

			for (int j = 0; j < 2; j++)
			{
				c.ExtractLR(j);

				c.m_Mm.Calculate(comm);
				m_pLR[c.m_iCycle][j] = comm;
				oracle << m_pLR[c.m_iCycle][j];
			}

			c.Condense();

			if (!c.m_iCycle)
				for (int j = 0; j < 2; j++)
					c.m_ppSrc[j] = c.m_pVal[j];
		}

		for (int i = 0; i < 2; i++)
			m_pCondensed[i] = c.m_pVal[i][0];
	}

	bool InnerProduct::IsValid(const Point::Native& commAB, const Scalar::Native& dotAB, const Modifier& mod) const
	{
		if (BatchContext::s_pInstance)
			return IsValid(*BatchContext::s_pInstance, commAB, dotAB, mod);

		BatchContextEx<1> bc;
		return
			IsValid(bc, commAB, dotAB, mod) &&
			bc.Flush();
	}

	bool InnerProduct::IsValid(BatchContext& bc, const Point::Native& commAB, const Scalar::Native& dotAB, const Modifier& mod) const
	{
		Mode::Scope scope(Mode::Fast);

		Oracle oracle;
		oracle << commAB;

		if (!bc.EquationBegin(1))
			return false;

		bc.AddCasual(commAB, 1U);

		return
			IsValid(bc, oracle, dotAB, mod) &&
			bc.EquationEnd();
	}

	bool InnerProduct::IsValid(BatchContext& bc, Oracle& oracle, const Scalar::Native& dotAB, const Modifier& mod) const
	{
		Mode::Scope scope(Mode::Fast);

		Calculator::ChallengeSet cs;
		oracle << dotAB >> cs.m_DotMultiplier;

		// Calculate the aggregated sum, consisting of sum of multiplications at once.
		// The expression we're calculating is:
		//
		// sum( LR[iCycle][0] * k[iCycle]^2 + LR[iCycle][0] * k[iCycle]^-2 )

		Calculator::ModifierExpanded modExp;
		modExp.Init(mod);

		Point::Native p;
		Scalar::Native k;

		uint32_t n = nDim;
		for (uint32_t iCycle = 0; iCycle < nCycles; iCycle++, n >>= 1)
		{
			Calculator::get_Challenge(cs.m_Val[iCycle], oracle);

			const Point* pLR = m_pLR[iCycle];
			for (int j = 0; j < 2; j++)
			{
				k = cs.m_Val[iCycle][j];
				k *= k;
				k = k;

				if (!bc.AddCasual(pLR[j], k))
					return false;

				oracle << pLR[j];
			}
		}

		assert(1 == n);

		// The expression we're calculating is: the transformed generator
		//
		// -sum( G_Condensed[j] * pCondensed[j] )
		// whereas G_Condensed[j] = Gen[j] * sum (k[iCycle]^(+/-)2 ), i.e. transformed (condensed) generators

		for (int j = 0; j < 2; j++)
		{
			MultiMac mmDummy;
			Calculator::Aggregator aggr(mmDummy, cs, modExp, j, 0);
			aggr.m_pBatchCtx = &bc;

			k = m_pCondensed[j];
			k = -k;

			if (bc.m_bEnableBatch)
				k *= bc.m_Multiplier;

			aggr.Proceed(0, nCycles, k);
		}

		// subtract the new (mutated) dot product, add the original (claimed)
		k = m_pCondensed[0];
		k *= m_pCondensed[1];
		k = -k;
		k += dotAB;
		k *= cs.m_DotMultiplier;

		bc.AddPrepared(BatchContext::s_Idx_GenDot, k);

		return true;
	}

	struct NonceGenerator
	{
		NoLeak<Oracle> m_Oracle;
		NoLeak<Scalar> m_sk;

		void operator >> (Scalar::Native& k)
		{
			NoLeak<Hash::Value> hv;
			m_Oracle.V >> hv.V;

			k.GenerateNonce(m_sk.V.m_Value, hv.V, NULL);
		}
	};

	/////////////////////
	// Bulletproof
	void RangeProof::Confidential::Create(const Scalar::Native& sk, Amount v, Oracle& oracle)
	{
		verify(CoSign(sk, v, oracle, Phase::SinglePass));
	}

	struct RangeProof::Confidential::MultiSig
	{
		Scalar::Native m_tau1;
		Scalar::Native m_tau2;

		void Init(NonceGenerator&);
		void Init(const Scalar::Native& sk, Amount v);

		void AddInfo1(Point::Native& ptT1, Point::Native& ptT2) const;
		void AddInfo2(Scalar::Native& taux, const Scalar::Native& sk, const ChallengeSet&) const;
	};

	struct RangeProof::Confidential::ChallengeSet
	{
		Scalar::Native x, y, z, zz;
		void Init(const Part1&, Oracle&);
		void Init(const Part2&, Oracle&);
	};

	bool RangeProof::Confidential::CoSign(const Scalar::Native& sk, Amount v, Oracle& oracle, Phase::Enum ePhase)
	{
		NonceGenerator nonceGen;
		nonceGen.m_sk.V = sk;
		nonceGen.m_Oracle.V << v;

		// A = G*alpha + vec(aL)*vec(G) + vec(aR)*vec(H)
		Scalar::Native alpha;
		nonceGen >> alpha;

		Point::Native comm = Context::get().G * alpha;

		{
			NoLeak<secp256k1_ge> ge;
			NoLeak<CompactPoint> ge_s;

			for (uint32_t i = 0; i < InnerProduct::nDim; i++)
			{
				uint32_t iBit = 1 & (v >> i);

				// protection against side-channel attacks
				object_cmov(ge_s.V, Context::get().m_Ipp.m_pGet1_Minus[i], 0 == iBit);
				object_cmov(ge_s.V, Context::get().m_Ipp.m_pGen_[0][i].m_Fast.m_pPt[0], 1 == iBit);

				Generator::ToPt(comm, ge.V, ge_s.V, false);
			}
		}

		m_Part1.m_A = comm;

		// S = G*ro + vec(sL)*vec(G) + vec(sR)*vec(H)
		Scalar::Native ro;
		nonceGen >> ro;

		MultiMac_WithBufs<1, InnerProduct::nDim * 2 + 1> mm;
		mm.m_pKPrep[mm.m_Prepared] = ro;
		mm.m_ppPrepared[mm.m_Prepared++] = &Context::get().m_Ipp.G_;

		Scalar::Native pS[2][InnerProduct::nDim];

		for (int j = 0; j < 2; j++)
			for (uint32_t i = 0; i < InnerProduct::nDim; i++)
			{
				nonceGen >> pS[j][i];

				mm.m_pKPrep[mm.m_Prepared] = pS[j][i];
				mm.m_ppPrepared[mm.m_Prepared++] = &Context::get().m_Ipp.m_pGen_[j][i];
			}

		mm.Calculate(comm);

		m_Part1.m_S = comm;

		//if (Phase::Step1 == ePhase)
		//	return; // stop after A,S calculated

		// get challenges
		ChallengeSet cs;
		cs.Init(m_Part1, oracle);

		// calculate t1, t2 - parts of vec(L)*vec(R) which depend on (future) x and x^2.
		Scalar::Native t0(Zero), t1(Zero), t2(Zero);

		Scalar::Native l0, r0, rx, one(1U), two(2U), yPwr, zz_twoPwr;

		yPwr = one;
		zz_twoPwr = cs.zz;

		for (uint32_t i = 0; i < InnerProduct::nDim; i++)
		{
			uint32_t bit = 1 & (v >> i);

			l0 = -cs.z;
			if (bit)
				l0 += one;

			const Scalar::Native& lx = pS[0][i];

			r0 = cs.z;
			if (!bit)
				r0 += -one;

			r0 *= yPwr;
			r0 += zz_twoPwr;

			rx = yPwr;
			rx *= pS[1][i];

			zz_twoPwr *= two;
			yPwr *= cs.y;

			t0 += l0 * r0;
			t1 += l0 * rx;
			t1 += lx * r0;
			t2 += lx * rx;
		}

		MultiSig msig;
		msig.Init(nonceGen);

		if (Phase::Finalize != ePhase) // otherwise m_Part2 already contains the whole aggregate
		{
			Point::Native comm2;
			msig.AddInfo1(comm, comm2);

			comm += Context::get().H_Big * t1;
			comm2 += Context::get().H_Big * t2;

			if (Phase::SinglePass != ePhase)
			{
				Point::Native p;
				if (!p.Import(m_Part2.m_T1))
					return false;
				comm += p;

				if (!p.Import(m_Part2.m_T2))
					return false;
				comm2 += p;
			}

			m_Part2.m_T1 = comm;
			m_Part2.m_T2 = comm2;
		}

		if (Phase::Step2 == ePhase)
			return true; // stop after T1,T2 calculated

		cs.Init(m_Part2, oracle); // get challenge 

		// m_TauX = tau2*x^2 + tau1*x + sk*z^2
		msig.AddInfo2(l0, sk, cs);

		if (Phase::SinglePass != ePhase)
			l0 += m_Part3.m_TauX;

		m_Part3.m_TauX = l0;

		// m_Mu = alpha + ro*x
		l0 = ro;
		l0 *= cs.x;
		l0 += alpha;
		m_Mu = l0;

		// m_tDot
		l0 = t0;

		r0 = t1;
		r0 *= cs.x;
		l0 += r0;

		r0 = t2;
		r0 *= cs.x;
		r0 *= cs.x;
		l0 += r0;

		m_tDot = l0;

		// construct vectors l,r, use buffers pS
		// P - m_Mu*G
		yPwr = one;
		zz_twoPwr = cs.zz;

		for (uint32_t i = 0; i < InnerProduct::nDim; i++)
		{
			uint32_t bit = 1 & (v >> i);

			pS[0][i] *= cs.x;

			pS[0][i] += -cs.z;
			if (bit)
				pS[0][i] += one;

			pS[1][i] *= cs.x;
			pS[1][i] *= yPwr;

			r0 = cs.z;
			if (!bit)
				r0 += -one;

			r0 *= yPwr;
			r0 += zz_twoPwr;

			pS[1][i] += r0;

			zz_twoPwr *= two;
			yPwr *= cs.y;
		}

		yPwr.SetInv(cs.y);

		InnerProduct::Modifier mod;
		mod.m_pMultiplier[1] = &yPwr;

		m_P_Tag.Create(oracle, l0, pS[0], pS[1], mod);

		return true;
	}

	void RangeProof::Confidential::MultiSig::Init(const Scalar::Native& sk, Amount v)
	{
		NonceGenerator nonceGen;
		nonceGen.m_sk.V = sk;
		nonceGen.m_Oracle.V << v;

		Init(nonceGen);
	}

	void RangeProof::Confidential::MultiSig::Init(NonceGenerator& nonceGen)
	{
		nonceGen >> m_tau1;
		nonceGen >> m_tau2;
	}

	void RangeProof::Confidential::MultiSig::AddInfo1(Point::Native& ptT1, Point::Native& ptT2) const
	{
		ptT1 = Context::get().G * m_tau1;
		ptT2 = Context::get().G * m_tau2;
	}

	void RangeProof::Confidential::MultiSig::AddInfo2(Scalar::Native& taux, const Scalar::Native& sk, const ChallengeSet& cs) const
	{
		// m_TauX = tau2*x^2 + tau1*x + sk*z^2
		taux = m_tau2;
		taux *= cs.x;
		taux *= cs.x;

		Scalar::Native t1 = m_tau1;
		t1 *= cs.x;
		taux += t1;

		t1 = cs.zz;
		t1 *= sk; // UTXO blinding factor (or part of it in case of multi-sig)
		taux += t1;
	}

	void RangeProof::Confidential::CoSignPart(const Scalar::Native& sk, Amount v, Oracle&, Part2& p2)
	{
		MultiSig msig;
		msig.Init(sk, v);

		Point::Native ptT1, ptT2;
		msig.AddInfo1(ptT1, ptT2);
		p2.m_T1 = ptT1;
		p2.m_T2 = ptT2;
	}

	void RangeProof::Confidential::CoSignPart(const Scalar::Native& sk, Amount v, Oracle& oracle, const Part1& p1, const Part2& p2, Part3& p3)
	{
		MultiSig msig;
		msig.Init(sk, v);

		ChallengeSet cs;
		cs.Init(p1, oracle);
		cs.Init(p2, oracle);

		Scalar::Native taux;
		msig.AddInfo2(taux, sk, cs);

		p3.m_TauX = taux;
	}

	void RangeProof::Confidential::ChallengeSet::Init(const Part1& p1, Oracle& oracle)
	{
		oracle << p1.m_A << p1.m_S;
		oracle >> y;
		oracle >> z;

		zz = z;
		zz *= z;
	}

	void RangeProof::Confidential::ChallengeSet::Init(const Part2& p2, Oracle& oracle)
	{
		oracle << p2.m_T1 << p2.m_T2;
		oracle >> x;
	}

	bool RangeProof::Confidential::IsValid(const Point::Native& commitment, Oracle& oracle) const
	{
		if (InnerProduct::BatchContext::s_pInstance)
			return IsValid(commitment, oracle, *InnerProduct::BatchContext::s_pInstance);

		InnerProduct::BatchContextEx<1> bc;
		bc.m_bEnableBatch = true; // why not?

		return
			IsValid(commitment, oracle, bc) &&
			bc.Flush();
	}

	bool RangeProof::Confidential::IsValid(const Point::Native& commitment, Oracle& oracle, InnerProduct::BatchContext& bc) const
	{
		Mode::Scope scope(Mode::Fast);

		if (bc.m_bEnableBatch)
		{
			Oracle o;

			for (uint32_t j = 0; j < 2; j++)
			{
				o << m_P_Tag.m_pCondensed[j];

				for (uint32_t i = 0; i < InnerProduct::nCycles; i++)
					o << m_P_Tag.m_pLR[i][j];
			}

			o
				<< m_Part1.m_A
				<< m_Part1.m_S
				<< m_Part2.m_T1
				<< m_Part2.m_T2
				<< m_Part3.m_TauX
				<< m_Mu
				<< m_tDot
				>> bc.m_Multiplier;
		}

		ChallengeSet cs;
		cs.Init(m_Part1, oracle);
		cs.Init(m_Part2, oracle);

		Scalar::Native xx, zz, tDot;

		// calculate delta(y,z) = (z - z^2) * sumY - z^3 * sum2
		Scalar::Native delta, sum2, sumY;


		sum2 = 1U;
		sumY = Zero;
		for (uint32_t i = 0; i < InnerProduct::nDim; i++)
		{
			sumY += sum2;
			sum2 *= cs.y;
		}

		sum2 = Amount(-1);

		zz = cs.z * cs.z;

		delta = cs.z;
		delta += -zz;
		delta *= sumY;

		sum2 *= zz;
		sum2 *= cs.z;
		delta += -sum2;

		// H_Big * m_tDot + G * m_TauX =?= commitment * z^2 + H_Big * delta(y,z) + m_T1*x + m_T2*x^2
		// H_Big * (m_tDot - delta(y,z)) + G * m_TauX =?= commitment * z^2 + m_T1*x + m_T2*x^2


		xx = cs.x * cs.x;

		Point::Native p;

		if (!bc.EquationBegin(3))
			return false;

		bc.AddCasual(commitment, -zz);
		if (!bc.AddCasual(m_Part2.m_T1, -cs.x))
			return false;
		if (!bc.AddCasual(m_Part2.m_T2, -xx))
			return false;

		tDot = m_tDot;
		sumY = tDot;
		sumY += -delta;

		bc.AddPrepared(InnerProduct::BatchContext::s_Idx_G, m_Part3.m_TauX);
		bc.AddPrepared(InnerProduct::BatchContext::s_Idx_H, sumY);

		if (!bc.EquationEnd())
			return false;

		// (P - m_Mu*G) + m_Mu*G =?= m_A + m_S*x - vec(G)*vec(z) + vec(H)*( vec(z) + vec(z^2*2^n*y^-n) )

		if (bc.m_bEnableBatch)
			Oracle() << bc.m_Multiplier >> bc.m_Multiplier;

		if (!bc.EquationBegin(2))
			return false;

		bc.AddPrepared(InnerProduct::BatchContext::s_Idx_Aux2, cs.z);
		bc.AddPrepared(InnerProduct::BatchContext::s_Idx_G, -Scalar::Native(m_Mu));
		if (!bc.AddCasual(m_Part1.m_S, cs.x))
			return false;

		Scalar::Native yInv, pwr, mul;
		yInv.SetInv(cs.y);

		mul = 2U;
		mul *= yInv;
		pwr = zz;

		for (uint32_t i = 0; i < InnerProduct::nDim; i++)
		{
			sum2 = pwr;
			sum2 += cs.z;

			bc.AddPrepared(InnerProduct::nDim + i, sum2);

			pwr *= mul;
		}

		bc.AddCasual(m_Part1.m_A, 1U);


		// By now the ptVal should be equal to the commAB
		// finally check the inner product
		InnerProduct::Modifier mod;
		mod.m_pMultiplier[1] = &yInv;

		if (!m_P_Tag.IsValid(bc, oracle, tDot, mod))
			return false;

		return bc.EquationEnd();
	}

	int RangeProof::Confidential::cmp(const Confidential& x) const
	{
		// don't care
		return memcmp(this, &x, sizeof(*this));
	}

} // namespace ECC
